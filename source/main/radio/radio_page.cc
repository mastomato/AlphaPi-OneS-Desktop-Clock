#include "radio_page.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display/display.h"
#include "display/lcd_display.h"
#include "lvgl.h"
#include "player.h"
#include "station_store.h"
#include "stations.h"

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace radio {
namespace {

constexpr char kTag[] = "RadioPage";

// 与待机时钟一致：给顶部状态栏留出 22 px，电量/网络图标不能被挡住。
constexpr int kTopBarReserve = 22;

lv_obj_t* g_root = nullptr;
lv_obj_t* g_station = nullptr;
lv_obj_t* g_index = nullptr;
lv_obj_t* g_state = nullptr;
lv_obj_t* g_volume = nullptr;
bool g_visible = false;
// 非空时覆盖状态行（用于把初始化失败的原因直接显示在屏上）
const char* g_status = nullptr;

// 电台名里只有 ASCII 部分能显示（内置字库无汉字），取出来当标识；
// 全中文的名字退化成 "CH"，靠序号区分。
std::string AsciiTag(const std::string& name) {
    std::string out;
    bool pending_space = false;
    for (unsigned char c : name) {
        if (c < 0x20 || c >= 0x7f) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(c));
        if (out.size() >= 12) {
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out.empty() ? std::string("CH") : out;
}

const char* StateText(PlayerState state) {
    switch (state) {
        case PlayerState::kPlaying: return "PLAYING";
        case PlayerState::kBuffering: return "BUFFERING";
        case PlayerState::kPaused: return "PAUSED";
        case PlayerState::kError: return "STREAM ERROR";
        default: return "STANDBY";
    }
}

void BuildUi(lv_obj_t* parent) {
    if (g_root != nullptr || parent == nullptr) {
        return;
    }

    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LV_PCT(100), LV_VER_RES - kTopBarReserve);
    lv_obj_set_pos(g_root, 0, kTopBarReserve);
    lv_obj_set_style_bg_color(g_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    g_visible = false;

    // 电台标识（ASCII 片段）
    g_station = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_station, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(g_station, lv_color_make(0xB4, 0xB4, 0xB4), 0);
    lv_label_set_text(g_station, "RADIO");
    lv_obj_align(g_station, LV_ALIGN_TOP_MID, 0, 4);

    // 序号：01/47
    g_index = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_index, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(g_index, lv_color_white(), 0);
    lv_label_set_text(g_index, "01/47");
    lv_obj_align(g_index, LV_ALIGN_CENTER, 0, -10);

    // 播放状态
    g_state = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_state, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(g_state, lv_color_make(0x7C, 0xFC, 0x9A), 0);
    lv_label_set_text(g_state, "STANDBY");
    lv_obj_align(g_state, LV_ALIGN_BOTTOM_MID, 0, -22);

    // 音量
    g_volume = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_volume, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(g_volume, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_label_set_text(g_volume, "VOL 70");
    lv_obj_align(g_volume, LV_ALIGN_BOTTOM_MID, 0, -4);

    ESP_LOGI(kTag, "radio page created");
}

/*
 * 建 UI 的唯一入口：**按需创建**。
 *
 * 开机阶段完全不建任何 LVGL 对象，等用户长按 D 真正进收音机时才建。原因：
 *   1. 这块板启动时经常卡在 TLS 握手重试里，显示锁长期被占，3 秒定时器那次
 *      创建几乎必然失败 —— 页面就永远不存在，长按 D 看起来毫无反应；
 *   2. 启动阶段少建一层全屏容器也少占一点内存（内部堆只剩几 KB 量级）。
 * 返回 false 表示当前拿不到显示锁，调用方可稍后重试。
 */
bool EnsureBuilt() {
    if (g_root != nullptr) {
        return true;
    }
    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return false;
    }
    DisplayLockGuard lock(display);
    if (!lock) {
        return false;
    }
    BuildUi(lv_screen_active());
    if (g_root == nullptr) {
        return false;
    }
    // 500 ms 的刷新跑在 LVGL 任务里，不用额外加锁
    lv_timer_create([](lv_timer_t*) { RadioPageRefresh(); }, 500, nullptr);
    ESP_LOGI(kTag, "radio page built on demand");
    return true;
}

}  // namespace

void RadioPageStart() {
    // 故意留空：页面改为「长按 D 时按需创建」，见 EnsureBuilt() 的说明。
}

void RadioPageShow(bool show) {
    if (show && !EnsureBuilt()) {
        ESP_LOGW(kTag, "radio page not built yet");
        return;
    }
    if (g_root == nullptr || show == g_visible) {
        return;
    }
    Display* display = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display);
    if (!lock) {
        return;
    }
    g_visible = show;
    if (show) {
        RadioPageRefresh();
        lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_root);
    } else {
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool RadioPageVisible() { return g_visible; }

void RadioPageSetStatus(const char* text) {
    g_status = text;
    RadioPageRefresh();
}

void RadioPageRefresh() {
    if (g_root == nullptr) {
        return;
    }
    auto status = Player::Instance().StatusSnapshot();
    const size_t count = StationCount();
    const size_t index = Player::Instance().station_index();
    const Station station = StationAt(index);

    lv_label_set_text(g_station, AsciiTag(station.name).c_str());

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02u/%02u", static_cast<unsigned>(index + 1),
                  static_cast<unsigned>(count));
    lv_label_set_text(g_index, buf);

    lv_label_set_text(g_state, g_status != nullptr ? g_status : StateText(status.state));

    std::snprintf(buf, sizeof(buf), "VOL %d", LoadVolume());
    lv_label_set_text(g_volume, buf);
}

}  // namespace radio
