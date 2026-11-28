#pragma once

#include <cstdint>

namespace radio {

/*
 * 播放器与板载音频 codec 之间的适配层。
 *
 * 2.4.2 时代收音机自带一套 UART 协议实现（audio_uart.cc）；到了 2.5.0，厂家已经把
 * N32 的全部协议细节做进 AlphaPiOneAudioCodec，包括「不发 WAV 头」「0x10=1 才是录音」
 * 「半双工切换要清 RX」这些踩过的坑。所以这里不再重复实现，只把播放器的调用转成
 * codec->WritePcm()。
 */
class PcmSink {
public:
    static PcmSink& Instance();

    // 绑定 codec 并把 N32 切到播放态（停麦克风、设音量）。可重复调用。
    bool Init();

    // 送入 24000 Hz / 单声道 / s16 PCM，返回实际被接受的样本数（0 表示失败）。
    int Write(const int16_t* data, int samples);

    // 播放器沿用旧接口名，语义等价于「确保麦克风关闭」——收音机播放期间必须如此。
    void StopRecord();

    // 本 codec 没有流内状态，保留空实现只为不改播放器的调用点。
    void InvalidateStream();

    bool ApplyPendingVolume();
};

}  // namespace radio
