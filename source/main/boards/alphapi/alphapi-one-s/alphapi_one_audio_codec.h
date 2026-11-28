#ifndef _ALPHAPI_ONE_AUDIO_CODEC_H
#define _ALPHAPI_ONE_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/uart.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <vector>

/*
 * AlphaPi One S 的音频链路不是 I2S codec，而是板载 N32 协处理器：
 *   ESP32-S2 ---(UART1 460800 8N1，TX=GPIO3 / RX=GPIO0)---> N32（内含功放）---> 板载喇叭
 *
 * 协议来自《AlphaPi-One-S-完整技术手册》§6 + 2026-11 实测（已修正手册里含糊的地方）：
 *   控制帧      : 90 <cmd> <len> <payload...> <checksum>        checksum = 前面所有字节累加 & 0xFF
 *   应答        : 91 <cmd> <accepted>                            PCM 写入时 accepted = 实际接受字节数，可能为 0
 *   麦克风读取  : 前缀 80 11 <size>   →   应答 81 11 <actual> <payload...> <checksum>
 *   命令        : 0x10 音频控制(1B, **1=开始录音, 0=停止/播放**) | 0x11 麦克风读取
 *                 0x14 硬件音量(1B,0..100) | 0x15 PCM 写入 | 0x0C 链路控制(写 2，同官方)
 *   播放流程    : 先停录音(0x10=0) → 清 RX → 设音量 → **直接发 ≤200 字节 8bit PCM**
 *                 （⚠️ 不要发 WAV 头！data 长度 0xFFFFFFFF 会让 N32 静音）
 *                 accepted==0 时短暂等待，用 accepted 数从断点补发余量
 *   采样格式    : 单声道 8bit；**播放 24000 Hz，录音 22050 Hz（两者不同，见 config.h）**
 *                 u8 → s16: (u8-128)*256，s16 → u8: (s16+32768)>>8
 *
 * 半双工：播放期间绝不能再发麦克风读取帧（手册 §6.4 明确要求）。
 * 连续对话：播放结束必须调 EnableInput(false)，否则框架不会再发 0x10=1（第二句没反应）。
 */
class AlphaPiOneAudioCodec : public AudioCodec {
public:
    AlphaPiOneAudioCodec(uart_port_t uart_num,
                         gpio_num_t tx_pin,
                         gpio_num_t rx_pin,
                         int input_sample_rate,
                         int output_sample_rate,
                         int baud_rate = 460800);
    virtual ~AlphaPiOneAudioCodec();

    virtual void Start() override;
    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

    /*
     * 网络收音机专用的直通口：把已重采样到 24000 Hz / 单声道 / s16 的 PCM 直接交给
     * Write()。Write() 是 protected（框架只让 AudioService 调），但收音机在待机态
     * 独占播放通道，需要一个公开入口。内部仍走完整协议：半双工切换、200 字节分块、
     * accepted==0 时从断点补发，不绕过任何一步。
     */
    int WritePcm(const int16_t* data, int samples) { return Write(data, samples); }

protected:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

private:
    // ---- 协议层 ----
    void ResetRxBuffer();
    void SendControlFrame(uint8_t cmd, const uint8_t* payload, size_t len);
    void SendFrameVariant(int f, uint8_t cmd, const uint8_t* payload, size_t len);
public:
    void SendTestFrame(uint8_t cmd, const uint8_t* payload, size_t len);
    int  ReadTestAck(uint8_t cmd, TickType_t timeout_ticks);
    /* 发送一帧并等待应答；返回应答第 3 字节 = 本次被 N32 接受的字节数（0=失败）。
     * 依据出厂 controlBoardOneS.mpy 里 machine_helper_one_s.uart_write 的字节码。 */
    int  DispatchFrame(uint8_t cmd, const uint8_t* payload, size_t len);
    /* 把音频数据完整送达：200 字节/帧，N32 只接受一部分就等 10ms 从断点补发。
     * 依据 uart_write_buf 的字节码（这是之前音频发不对的根因）。 */
    bool WriteAudioFlow(uint8_t addr, const uint8_t* data, size_t len);
private:
    // 读一个 91 <cmd> <accepted> 应答，返回 accepted；超时/协议错误返回 -1
    int ReadAck(uint8_t expected_cmd, TickType_t timeout_ticks);
    /* 注意：原来这里有个 SendWavHeader()（发 46 字节 WAV 头）。
     * 2026-10 实测：N32 收到 data 长度 0xFFFFFFFF 的头之后就不出声，
     * 所以正式版**不发任何头**，直接写 0x15 裸 PCM。函数已删除。 */
    void SetHardwareVolume(int hw_volume);
    void SetMicStream(bool enable);
    /* 与出厂 __tickRecord 一致：每次向 N32 要 200 字节（n=min(200, remaining)），
     * 数据落在 mic_pcm_ 里，返回解析到的字节数；多余部分留给下一次 Read 取用。 */
    int  RequestMicBlock();
    uint8_t mic_pcm_[200];
    int  mic_fill_ = 0;
    int  mic_take_ = 0;
    /* 麦克风原始 PCM dump（诊断用）：攒满一块写进 ota_1 尾部 0x5F0000，
     * 该位置在 app 镜像之后，不管从哪个槽启动都不会被破坏。 */
    /* 16KB 麦克风 dump 缓冲：放 PSRAM。
     * 之前是 BSS 里的 16KB 内部 RAM，直接吃掉本来就只剩 8~15KB 的内部堆，
     * 导致 mbedTLS 握手间歇性失败（"检查新版本"要重试约 10 次才成功）。 */
    static constexpr int kMicDumpBytes = 16384;
    uint8_t* mic_dump_ = nullptr;   /* PSRAM 分配，见 Start() */
    int     mic_dump_len_ = 0;

    static uint8_t Checksum(const uint8_t* data, size_t len);

    // 播放会话：最后一次 Write 之后静音超过 kPlaybackIdleMs 即认为本次播放结束
    void MarkPlaybackActivity();
    static void PlaybackIdleCallback(void* arg);

private:
    static constexpr int kPlaybackIdleMs = 3000;  /* 250->3000：阈值太小会在段间静音时重发 WAV 头，导致 N32 重新初始化流 ⇒ 噪声 */   // [需实测] 这个阈值决定“何时重发 WAV 头”
    static constexpr int kAckTimeoutMs   = 300;   // [需实测] 单块 PCM 的应答超时

    uart_port_t uart_num_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    int baud_rate_;
    bool uart_ready_ = false;

    /* I2S 音频数据通道（手册 §11：控制走 UART，音频数据走 I2S）
     * 引脚取自手册：BCLK=5, WS=4, DOUT=7, DIN=6（与 P1 扩展口 4/5/6/7 吻合）*/
    i2s_chan_handle_t i2s_tx_ = nullptr;
    i2s_chan_handle_t i2s_rx_ = nullptr;
    bool i2s_ready_ = false;


    bool mic_streaming_ = false;      // 0x10 已开
    int  mic_stall_count_ = 0;        // 连续取不到麦克数据的次数（录音态自恢复用）
    bool playback_session_ = false;   // 本次播放是否已经发过 WAV 头
    esp_timer_handle_t playback_idle_timer_ = nullptr;

    std::vector<uint8_t> pcm_u8_;     // s16 → u8 的转换缓冲，避免每次播放重新分配
};

#endif // _ALPHAPI_ONE_AUDIO_CODEC_H
