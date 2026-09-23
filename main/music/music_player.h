#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class AudioService;

/**
 * MusicPlayer —— 从 http(s) 直链流式播放 MP3 音乐（用于酷狗等音乐直链）。
 *
 * 数据流：
 *   HTTP(MP3) --esp_mp3_dec 解码--> 单声道化 --> 重采样到喇叭采样率
 *             --> AudioService::PushPcmToPlaybackQueue()
 *             --> 设备原有的 audio_output 任务 --> 喇叭
 *
 * 关键设计：音乐复用设备**本来就在用**的播放通道（与语音播报、云端通知播放
 * 完全同一条队列），不另起一套 I2S / 写码器逻辑，因此不会像第三方固件那样
 * 把设备音频通道弄哑。
 *
 * 取消机制：Stop() 只置取消标志；同时 Application 会调用
 * AudioService::ResetDecoder()（递增播放代次），使排在队列里的 PCM 立即失效，
 * 播放任务随后自然退出。所以“停止播放”永远能立刻生效，不会卡死。
 */
class MusicPlayer {
public:
    using FinishedCallback = std::function<void(bool success)>;

    explicit MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();

    /**
     * 开始播放。立即返回，真正的拉流/解码在独立任务里进行。
     * @param audio_url  http/https 音频直链（MP3）
     * @param title      歌曲名，仅用于日志与状态查询
     * @param finished_callback 播放结束（或被取消）时回调，在播放任务里执行
     * @return 是否成功启动（地址非法或已有播放任务时返回 false）
     */
    bool Start(std::string audio_url, std::string title, FinishedCallback finished_callback);

    /** 请求停止播放（异步，不阻塞调用者）。 */
    void Stop();

    /** 是否正在播放中。 */
    bool IsPlaying() const;

    /** 是否正在播放或播放任务尚未退出。 */
    bool IsBusy() const;

    /** 正在播放的歌曲名，未播放时返回空串。 */
    std::string GetTitle() const;

private:
    static void WorkerEntry(void* arg);
    void WorkerTask();

    AudioService& audio_service_;

    mutable std::mutex mutex_;
    std::string audio_url_;
    std::string title_;
    FinishedCallback finished_callback_;
    std::atomic<bool> active_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> worker_running_{false};
    TaskHandle_t task_handle_ = nullptr;
};

#endif  // MUSIC_PLAYER_H
