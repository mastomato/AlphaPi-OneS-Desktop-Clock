#include "radio_controller.h"

#include <algorithm>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "pcm_sink.h"
#include "player.h"
#include "radio_page.h"
#include "station_store.h"
#include "stations.h"

namespace radio {
namespace {
constexpr char kTag[] = "RadioCtrl";

}  // namespace

RadioController& RadioController::Instance() {
    static RadioController instance;
    return instance;
}

void RadioController::Init() {
    /*
     * 只建 UI（一个 3 秒后的 esp_timer），**不在这里初始化播放器**。
     *
     * 原因：ESP32-S2 是单核，播放器一初始化就会创建三个任务（优先级 5/6/7）
     * 并申请约 450 KB 的队列缓冲。如果把它放在板卡构造函数里，这些任务会在
     * Wi-Fi / LVGL 初始化期间就参与调度，实测会把启动流程卡在"正在初始化"。
     * 改成第一次真正进收音机时再初始化，启动阶段完全不碰它。
     */
    RadioPageStart();
}

void RadioController::Enter() {
    if (active_) {
        return;
    }
    // 先亮出电台页：长按 D 必须立刻有视觉反馈，否则用户只会觉得"按了没反应"。
    // 页面按需创建，若此刻显示锁被占就重试几次（最多约 1 秒）。
    for (int i = 0; i < 10 && !RadioPageVisible(); ++i) {
        RadioPageShow(true);
        if (RadioPageVisible()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    RadioPageSetStatus("INIT...");
    // 先置位：即使后面初始化失败，长按 D 也能退出，不会把用户卡在电台页
    active_ = true;

    // 播放器延迟到这里才初始化（见 Init() 的说明）。
    static bool player_ready = false;
    if (!player_ready) {
        player_ready = Player::Instance().Init();
        if (!player_ready) {
            ESP_LOGE(kTag, "player init failed");
            RadioPageSetStatus("INIT FAIL");
            return;
        }
    }

    // 收音机独占播放通道：把应用压回待机，避免它同时喂 PCM 给 N32。
    Application::GetInstance().SetDeviceState(kDeviceStateIdle);

    if (!PcmSink::Instance().Init()) {
        ESP_LOGE(kTag, "audio sink init failed");
        RadioPageSetStatus("AUDIO FAIL");
        return;
    }
    const int volume = LoadVolume();
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        codec->SetOutputVolume(muted_ ? 0 : volume);
    }

    RadioPageSetStatus(nullptr);
    Player::Instance().Play(Player::Instance().station_index());
    ESP_LOGI(kTag, "radio on, station %u", static_cast<unsigned>(Player::Instance().station_index() + 1));
}

void RadioController::Exit() {
    if (!active_) {
        return;
    }
    Player::Instance().Stop();
    active_ = false;
    RadioPageShow(false);
    // 交回麦克风给下一轮对话；框架的 ReadAudioData 只在 input 未使能时才会重开，
    // 所以这里显式使能，避免「第二句没反应」。
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        codec->EnableInput(true);
    }
    ESP_LOGI(kTag, "radio off");
}

void RadioController::Toggle() {
    if (active_) {
        Exit();
    } else {
        Enter();
    }
}

void RadioController::PlayPause() {
    if (!active_) {
        return;
    }
    Player::Instance().Toggle();
}

void RadioController::Step(int delta) {
    if (!active_) {
        return;
    }
    const int count = static_cast<int>(StationCount());
    if (count <= 0) {
        return;
    }
    int next = static_cast<int>(Player::Instance().station_index()) + delta;
    while (next < 0) next += count;
    next %= count;
    Player::Instance().Play(static_cast<size_t>(next));
    RadioPageRefresh();
}

void RadioController::ToggleMute() {
    if (!active_) {
        return;
    }
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    if (!muted_) {
        volume_before_mute_ = LoadVolume();
        muted_ = true;
        codec->SetOutputVolume(0);
    } else {
        muted_ = false;
        codec->SetOutputVolume(volume_before_mute_);
    }
}

}  // namespace radio
