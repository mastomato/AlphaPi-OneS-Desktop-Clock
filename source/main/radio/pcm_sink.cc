#include "pcm_sink.h"

#include <esp_log.h>

#include "board.h"
#include "boards/alphapi/alphapi-one-s/alphapi_one_audio_codec.h"

namespace radio {
namespace {
constexpr char kTag[] = "PcmSink";

AlphaPiOneAudioCodec* GetCodec() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    // 本固件只支持这一块板，codec 一定是 AlphaPi 的实现。
    return static_cast<AlphaPiOneAudioCodec*>(codec);
}
}  // namespace

PcmSink& PcmSink::Instance() {
    static PcmSink instance;
    return instance;
}

bool PcmSink::Init() {
    auto* codec = GetCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "no audio codec");
        return false;
    }
    // 收音机播放是独占的：先关麦克风，否则 N32 停在录音态，播放会静音。
    codec->EnableInput(false);
    codec->EnableOutput(true);
    return true;
}

int PcmSink::Write(const int16_t* data, int samples) {
    auto* codec = GetCodec();
    if (codec == nullptr || data == nullptr || samples <= 0) {
        return 0;
    }
    return codec->WritePcm(data, samples);
}

void PcmSink::StopRecord() {
    auto* codec = GetCodec();
    if (codec != nullptr) {
        codec->EnableInput(false);
    }
}

void PcmSink::InvalidateStream() {
    // codec 自己按「空闲 3 秒」判定一次播放会话结束，这里无需额外动作。
}

bool PcmSink::ApplyPendingVolume() {
    return true;
}

}  // namespace radio
