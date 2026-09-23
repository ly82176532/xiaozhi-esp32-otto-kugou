#include "music_player.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>
#include <esp_timer.h>

#include "audio_service.h"
#include "board.h"
#include "http.h"

namespace {
const char* TAG = "MusicPlayer";

constexpr int kHttpTimeoutMs = 5000;
// 播放任务：优先级略低于 audio_output(4)，保证喇叭端始终优先拿到 CPU
constexpr uint32_t kTaskStackSize = 8192;
constexpr UBaseType_t kTaskPriority = 3;

// 网络输入缓冲：优先放 PSRAM（本板 sdkconfig 已开 CONFIG_SPIRAM）
constexpr size_t kInputCapacityPsram = 128 * 1024;
constexpr size_t kInputCapacityInternal = 32 * 1024;
constexpr size_t kReadChunk = 4096;
// 单帧 MP3 最多 1152 样本 × 2 声道 × 2 字节 = 4608 字节，留足余量
constexpr size_t kPcmOutBytesInitial = 8192;

// 结尾等待“喇叭把尾巴放完”的兜底上限
constexpr int64_t kFlushTimeoutUs = 30LL * 1000 * 1000;
constexpr int64_t kFlushPollIntervalMs = 50;

#define MUSIC_RATE_CVT_CFG(_src_rate, _dest_rate, _channel) \
    (esp_ae_rate_cvt_cfg_t)                                 \
    {                                                       \
        .src_rate        = (uint32_t)(_src_rate),           \
        .dest_rate       = (uint32_t)(_dest_rate),          \
        .channel         = (uint8_t)(_channel),             \
        .bits_per_sample = ESP_AUDIO_BIT16,                 \
        .complexity      = 2,                               \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED, \
    }

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

// 计算文件开头 ID3v2 标签要跳过的字节数；不是 ID3v2 时返回 0。
size_t Id3v2SkipSize(const uint8_t* data, size_t size) {
    if (size < 10 || memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    if (data[3] == 0xFF && data[4] == 0xFF) {
        // 版本号非法，按普通数据交给解码器找同步
        return 0;
    }
    // 标签长度使用“同步安全整数”：每字节只用低 7 位
    const size_t tag_size = ((size_t)(data[6] & 0x7F) << 21) |
                            ((size_t)(data[7] & 0x7F) << 14) |
                            ((size_t)(data[8] & 0x7F) << 7) |
                            (size_t)(data[9] & 0x7F);
    size_t total = 10 + tag_size;
    if ((data[5] & 0x10) != 0) {
        total += 10;  // 带 footer
    }
    return total;
}
}  // namespace

MusicPlayer::MusicPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

MusicPlayer::~MusicPlayer() { Stop(); }

bool MusicPlayer::Start(std::string audio_url, std::string title, FinishedCallback finished_callback) {
    if (!IsSupportedUrl(audio_url)) {
        ESP_LOGE(TAG, "Unsupported music url");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            ESP_LOGW(TAG, "Music player is busy");
            return false;
        }
        audio_url_ = std::move(audio_url);
        title_ = std::move(title);
        finished_callback_ = std::move(finished_callback);
        cancelled_ = false;
        active_ = true;
        worker_running_ = true;
    }

    const BaseType_t created = xTaskCreate(WorkerEntry, "music_play", kTaskStackSize, this, kTaskPriority,
                                           &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void MusicPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    finished_callback_ = nullptr;
}

bool MusicPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool MusicPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

std::string MusicPlayer::GetTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? title_ : std::string();
}

void MusicPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<MusicPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::WorkerTask() {
    std::string audio_url;
    std::string title;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        title = title_;
    }
    ESP_LOGI(TAG, "Start music playback: %s", title.empty() ? audio_url.c_str() : title.c_str());

    bool success = false;
    bool played_any = false;

    size_t capacity = kInputCapacityPsram;
    auto* input = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (input == nullptr) {
        capacity = kInputCapacityInternal;
        input = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_DEFAULT));
        ESP_LOGW(TAG, "PSRAM unavailable, input buffer falls back to %u bytes", (unsigned)capacity);
    }

    size_t input_len = 0;
    bool eof = false;
    bool id3_checked = false;

    void* mp3 = nullptr;
    std::vector<uint8_t> pcm_out(kPcmOutBytesInitial);
    std::vector<int16_t> mono;
    std::vector<int16_t> ready;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_src_rate = 0;

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);

    // 用 do{...}while(false) 代替 try/finally，任何一步失败都能走到统一清理
    do {
        if (input == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate music input buffer");
            break;
        }
        if (!http) {
            ESP_LOGE(TAG, "Failed to create music http client");
            break;
        }

        // 丢掉的音频数据（下一次播放、下一次解码都不需要）
        input_len = 0;
        eof = false;
        id3_checked = false;

        if (esp_mp3_dec_open(nullptr, 0, &mp3) != ESP_AUDIO_ERR_OK || mp3 == nullptr) {
            ESP_LOGE(TAG, "Failed to open mp3 decoder");
            mp3 = nullptr;
            break;
        }

        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "audio/mpeg, audio/mp3, audio/*, */*");
        http->SetHeader("Accept-Encoding", "identity");
        http->SetHeader("User-Agent", "Mozilla/5.0 (XiaozhiMusic)");
        if (!http->Open("GET", audio_url)) {
            ESP_LOGE(TAG, "Music http open failed: %d", http->GetLastError());
            break;
        }
        const int status = http->GetStatusCode();
        if (status < 200 || status >= 300) {
            ESP_LOGE(TAG, "Music http returned status %d", status);
            break;
        }

        const auto consume = [&](size_t count) {
            if (count >= input_len) {
                input_len = 0;
                return;
            }
            memmove(input, input + count, input_len - count);
            input_len -= count;
        };

        const auto read_more = [&]() -> size_t {
            if (eof || input_len >= capacity) {
                return 0;
            }
            const size_t want = std::min(capacity - input_len, kReadChunk);
            const int read = http->Read(reinterpret_cast<char*>(input + input_len), want);
            if (read < 0) {
                ESP_LOGW(TAG, "Music http read failed: %d", http->GetLastError());
                eof = true;
                return 0;
            }
            if (read == 0) {
                eof = true;
                return 0;
            }
            input_len += (size_t)read;
            return (size_t)read;
        };

        // 预缓冲：先攒够一段再开播，避免一开头就断流
        const size_t prebuffer = capacity / 4 * 3;
        while (!cancelled_.load() && !eof && input_len < prebuffer) {
            if (read_more() == 0) {
                break;
            }
        }
        if (cancelled_.load()) {
            break;
        }
        ESP_LOGI(TAG, "Prebuffered %u bytes", (unsigned)input_len);

        while (!cancelled_.load()) {
            // 有空间就继续拉数据
            if (!eof && input_len + kReadChunk <= capacity) {
                read_more();
            }
            if (input_len == 0) {
                break;  // 流已读完
            }

            if (!id3_checked) {
                if (input_len < 10) {
                    // 头部数据还不够判断是不是 ID3v2，先让循环顶部去补数据
                    if (!eof) {
                        continue;
                    }
                    id3_checked = true;
                } else {
                    const size_t skip = Id3v2SkipSize(input, input_len);
                    if (skip == 0) {
                        id3_checked = true;
                    } else if (skip <= input_len) {
                        consume(skip);
                        id3_checked = true;
                        ESP_LOGI(TAG, "Skipped ID3v2 tag: %u bytes", (unsigned)skip);
                        continue;
                    } else {
                        consume(input_len);  // 标签还没读全，先腾出空间继续读
                        continue;
                    }
                }
            }

            esp_audio_dec_in_raw_t raw = {
                .buffer = input,
                .len = (uint32_t)input_len,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t frame = {
                .buffer = pcm_out.data(),
                .len = (uint32_t)pcm_out.size(),
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {};
            const esp_audio_err_t ret = esp_mp3_dec_decode(mp3, &raw, &frame, &info);

            if (raw.consumed > 0) {
                consume(raw.consumed);
            }

            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(TAG, "Mp3 output buffer too small, growing to %u", (unsigned)(pcm_out.size() * 2));
                pcm_out.resize(pcm_out.size() * 2);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (raw.consumed == 0) {
                    // 数据不足一帧：先拉更多；实在拉不动就丢弃 1 字节重新找同步
                    if (!eof && input_len + kReadChunk <= capacity) {
                        continue;
                    }
                    consume(1);
                }
                continue;
            }

            const size_t total_samples = frame.decoded_size / sizeof(int16_t);
            if (total_samples == 0) {
                continue;
            }
            const int16_t* src = reinterpret_cast<const int16_t*>(pcm_out.data());
            const uint32_t channels = info.channel != 0 ? info.channel : 2;
            const uint32_t src_rate = info.sample_rate != 0 ? info.sample_rate : 44100;

            // 1) 多声道混成单声道
            if (channels >= 2) {
                const size_t frames_per_channel = total_samples / channels;
                mono.resize(frames_per_channel);
                for (size_t i = 0; i < frames_per_channel; ++i) {
                    int sum = 0;
                    for (uint32_t c = 0; c < channels; ++c) {
                        sum += src[i * channels + c];
                    }
                    mono[i] = (int16_t)(sum / (int)channels);
                }
            } else {
                mono.assign(src, src + total_samples);
            }
            if (mono.empty()) {
                continue;
            }

            // 2) 重采样到设备喇叭的采样率（复用工程已有的 esp_ae_rate_cvt）
            auto* codec = Board::GetInstance().GetAudioCodec();
            const uint32_t out_rate = (codec != nullptr && codec->output_sample_rate() > 0)
                                          ? (uint32_t)codec->output_sample_rate()
                                          : src_rate;
            if (src_rate == out_rate) {
                ready = std::move(mono);
            } else {
                if (resampler == nullptr || resampler_src_rate != (int)src_rate) {
                    if (resampler != nullptr) {
                        esp_ae_rate_cvt_close(resampler);
                        resampler = nullptr;
                    }
                    esp_ae_rate_cvt_cfg_t cfg = MUSIC_RATE_CVT_CFG(src_rate, out_rate, ESP_AUDIO_MONO);
                    esp_ae_rate_cvt_open(&cfg, &resampler);
                    if (resampler == nullptr) {
                        ESP_LOGE(TAG, "Failed to create music resampler %u -> %u", (unsigned)src_rate,
                                 (unsigned)out_rate);
                        break;
                    }
                    resampler_src_rate = (int)src_rate;
                    ESP_LOGI(TAG, "Music resampling %u -> %u", (unsigned)src_rate, (unsigned)out_rate);
                }
                uint32_t max_out = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &max_out);
                ready.resize(max_out);
                uint32_t actual_out = max_out;
                esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), (uint32_t)mono.size(),
                                        (esp_ae_sample_t)ready.data(), &actual_out);
                ready.resize(actual_out);
            }
            if (ready.empty()) {
                continue;
            }

            // 3) 交给设备原生播放通道（走不进去说明已被取消）
            if (!audio_service_.PushPcmToPlaybackQueue(std::move(ready), true)) {
                ESP_LOGI(TAG, "Music playback cancelled while pushing pcm");
                break;
            }
            ready.clear();
            mono.clear();
            played_any = true;
        }

        if (!cancelled_.load()) {
            // 等喇叭把队列里的尾巴放完，避免结尾被截断
            const int64_t deadline = esp_timer_get_time() + kFlushTimeoutUs;
            while (!cancelled_.load() && !audio_service_.IsPlaybackIdle() && esp_timer_get_time() < deadline) {
                vTaskDelay(pdMS_TO_TICKS(kFlushPollIntervalMs));
            }
            success = true;
        }
    } while (false);

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
        resampler = nullptr;
    }
    if (mp3 != nullptr) {
        esp_mp3_dec_close(mp3);
        mp3 = nullptr;
    }
    if (input != nullptr) {
        heap_caps_free(input);
        input = nullptr;
    }
    if (http) {
        http->Close();
        http.reset();
    }

    ESP_LOGI(TAG, "Music playback task ended: success=%d played=%d cancelled=%d", (int)success, (int)played_any,
             (int)cancelled_.load());

    FinishedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        active_ = false;
        if (!cancelled_.load()) {
            callback = std::move(finished_callback_);
        }
        finished_callback_ = nullptr;
    }
    if (callback) {
        callback(success && played_any);
    }
}
