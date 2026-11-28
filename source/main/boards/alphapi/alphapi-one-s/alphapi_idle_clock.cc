#include "alphapi_idle_clock.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "alphapi_weather.h"
#include "alphapi_weather_icon.h"
#include "application.h"
#include "board.h"
#include "display/display.h"

/*
 * 待机时钟页（AlphaPi One S）
 * ---------------------------
 * 版式：B 方案「左时右天 + 底部三天预报」（预览稿 D:\one s\首页排版预览-B版重排-20261112.html）
 *   bg #0E1420 · 主文字 #E6EEF7 · 强调青 #8FE3F0 · 暖黄 #FFD27A · 次要灰 #7186A0
 *
 *   左栏（x8..104）        右栏（x106..156，50px 宽，居中）
 *   ① 日期   屏 24..37     ④ 天气图标 32×32 @(115,26)
 *   ② 时间   屏 47..70     ⑤ 温度    屏 63..76
 *   ③ 城市   屏 81..94     ⑥ 天气描述 屏 81..94
 *   ─────────────────────────────────────────────
 *   ⑦ 三天预报 屏 104..116  今/明/后，每段「12×12 矢量图标 + 日名 + 最高温」
 *                            段起点 x 8/58/108
 *
 * 纵向按**墨迹**等距（不是 label 盒子）：三处间隙都是 10px，下边距 12px。
 * 上部三行比 v8 整体上移 5~9px，省出的底部空间正好容纳预报行。
 *
 * 未配置天气位置时：左栏城市格换成设备 IP（暖黄 14px，同格互斥、切换不跳动），
 * 右栏保留 --°C 占位，底部三天预报整体隐藏（不给远处默认位置的预报）。
 *
 * 字体：中文 alphapi_cn_16（simhei 子集）；时间 alphapi_time_32（数字+冒号+横线，
 * Segoe UI Bold，行高 24、数字墨迹占行内 1..24）；当前温度 alphapi_temp_16
 * （数字 - ° C，Segoe UI Bold 16px）；预报日名 alphapi_cn_12（今明后）；
 * 预报温度 alphapi_num_12（数字+度数）。全部由 lv_font_conv 生成、随源码编译，不动 Kconfig。
 * 注意 cn_16 不含 U+00B0，凡是要显示「°C」的 label 必须用 temp_16 / num_12。
 *
 * 时间为什么是 32px（C 版用 40px）：左栏只有 96px 宽，40px 的「21:47」实测约 100px
 * 会顶到分栏线；32px 约 82px，右侧还剩 14px。
 */

LV_FONT_DECLARE(alphapi_time_32);
LV_FONT_DECLARE(alphapi_cn_16);
LV_FONT_DECLARE(alphapi_cn_12);
LV_FONT_DECLARE(alphapi_num_12);
LV_FONT_DECLARE(alphapi_temp_16);
LV_FONT_DECLARE(lv_font_montserrat_14);

namespace {

constexpr char kTag[] = "IdleClock";

constexpr int kTopBarReserve = 22;

constexpr uint32_t kBg = 0x0E1420;
constexpr uint32_t kAccent = 0x8FE3F0;
constexpr uint32_t kPrimary = 0xE6EEF7;
constexpr uint32_t kMuted = 0x7186A0;
constexpr uint32_t kWarm = 0xFFD27A;
constexpr uint32_t kForecastTemp = 0xB8C4D6;

constexpr int kForecastSlots = 3;      // 今 / 明 / 后
constexpr int kForecastPitch = 50;     // 段起点 x 8 / 58 / 108（文字偏移 = 起点 +14/+28）

lv_obj_t* g_root = nullptr;
lv_obj_t* g_time = nullptr;
lv_obj_t* g_date = nullptr;
lv_obj_t* g_temp = nullptr;
lv_obj_t* g_desc = nullptr;
lv_obj_t* g_city = nullptr;
lv_obj_t* g_ip = nullptr;
lv_obj_t* g_icon = nullptr;
lv_obj_t* g_fc_day[kForecastSlots] = {};
lv_obj_t* g_fc_temp[kForecastSlots] = {};
lv_obj_t* g_fc_icon[kForecastSlots] = {};
bool g_visible = false;
bool g_sntp_started = false;

/*
 * 每秒 Tick 只改真正变化的内容。
 * ---------------------------------------------------------------------------
 * 之前 UpdateText() 无条件对 12 个 label 调 lv_label_set_text()、对 11 个对象
 * 调 add/remove_flag、还做 3 次 lv_obj_update_layout()。lv_label_set_text 即使
 * 文本相同也会重新分配并复制字符串、把对象标脏；ESP32-S2 没有 PSRAM，内部 RAM
 * 本来就紧，长时间运行后 LVGL 堆碎片化，最终分配失败 / 刷新卡住 —— 表现为
 * 「用一段时间有概率卡死」。这里用一层文本缓存把无谓的写操作全部挡掉。
 */
struct TextCache {
    char time[8] = {};
    char date[24] = {};
    char city[32] = {};
    char ip[16] = {};
    char temp[12] = {};
    char desc[16] = {};
    char fc_day[kForecastSlots][8] = {};
    char fc_temp[kForecastSlots][24] = {};
    int fc_icon_x[kForecastSlots] = {-1, -1, -1};
};
TextCache g_cache;

// 文本没变就什么都不做，返回 false；变了才写 LVGL 并更新缓存。
bool SetLabel(lv_obj_t* obj, char* cache, size_t n, const char* text) {
    if (obj == nullptr || cache == nullptr || text == nullptr) {
        return false;
    }
    if (std::strcmp(cache, text) == 0) {
        return false;
    }
    std::snprintf(cache, n, "%s", text);
    lv_label_set_text(obj, text);
    return true;
}

// 统一显隐；flag 已经是目标状态就不再动，避免每秒重复 invalidate。
void Show(lv_obj_t* obj, bool visible) {
    if (obj == nullptr) {
        return;
    }
    const bool is_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (visible == !is_hidden) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void EnsureTimezone() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    setenv("TZ", "CST-8", 1);
    tzset();
}

void StartSntpOnce() {
    if (g_sntp_started) {
        return;
    }
    g_sntp_started = true;
    EnsureTimezone();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(kTag, "SNTP started (ntp.aliyun.com, TZ=CST-8)");
}

/*
 * 离线时间基准（TLS 证书有效期校验的前提）
 * ----------------------------------------
 * 这块板的 SNTP 常年同步不成功（实测系统时间停在 1881/1/2），而 mbedTLS 校验
 * 服务器证书时会检查 notBefore/notAfter；时间落在证书生效日之前就会返回
 * BADCERT_FUTURE（0x801A），表现为「TLS handshake failed」并且重试多少次都一样。
 * 这不是内存问题，也不是间歇性抖动，所以调小 mbedTLS 缓冲区毫无帮助。
 *
 * 解法：不依赖任何网络，在 app_main 最早期、任何 HTTPS/WebSocket 连接之前，
 * 用**编译时刻**给系统设一个基准时间。编译时刻必然落在当前服务器证书的有效
 * 期内，证书校验因此必定通过。之后 SNTP 或天气 HTTP 的 Date 头会把时间校准到
 * 真实值（见 alphapi_weather.cc），两者谁先到都无妨。
 */
static const char* const kBuildDate = __DATE__;   // "Nov 11 2026"
static const char* const kBuildTime = __TIME__;   // "14:15:30"

// 编译机在 UTC+8，而 timegm() 按 UTC 解析，故减 8 小时换回真实 UTC 时间戳。
constexpr time_t kBuildTzOffset = 8 * 3600;

int MonthIndex(const char* mon) {
    static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(mon, kMonths[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

bool TimeReady(const struct tm& t) { return (t.tm_year + 1900) >= 2020; }

const char* WeekdayCn(int wd) {
    static const char* const kDays[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (wd < 0 || wd > 6) {
        wd = 0;
    }
    return kDays[wd];
}

const char* WeatherCn(int code) {
    if (code < 0) return "--";   /* 字库子集里没有「知」，用 -- 避免豆腐块 */
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code <= 48) return "雾";
    if (code <= 57) return "毛毛雨";
    if (code <= 67) return "雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    return "雷阵雨";
}

const char* ForecastDayCn(int idx) {
    static const char* const kNames[kForecastSlots] = {"今", "明", "后"};
    if (idx < 0 || idx >= kForecastSlots) {
        return "--";
    }
    return kNames[idx];
}

void UpdateText() {
    if (g_time == nullptr) {
        return;
    }
    /*
     * 系统时间统一保存为**纯 UTC**（SNTP / 天气 HTTP Date / OTA server_time
     * 写的都是 UTC 时间戳）。这里显式 +8 再用 gmtime_r 换算，不依赖 TZ 环境变量
     * 是否生效，也彻底避开「系统时间被写成北京时间、localtime_r 又加一次时区」
     * 的双重偏移。状态栏 lvgl_display.cc 用同样的算法，两处显示保持一致。
     */
    constexpr time_t kChinaUtcOffset = 8 * 3600;
    time_t now = time(nullptr) + kChinaUtcOffset;
    struct tm t = {};
    gmtime_r(&now, &t);

    char buf[40];
    if (!TimeReady(t)) {
        SetLabel(g_time, g_cache.time, sizeof(g_cache.time), "--:--");
        SetLabel(g_date, g_cache.date, sizeof(g_cache.date), "--/-- 周-");
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        SetLabel(g_time, g_cache.time, sizeof(g_cache.time), buf);
        std::snprintf(buf, sizeof(buf), "%d/%d %s", t.tm_mon + 1, t.tm_mday,
                      WeekdayCn(t.tm_wday));
        SetLabel(g_date, g_cache.date, sizeof(g_cache.date), buf);
    }

    /*
     * 显隐规则（B 方案）
     *   左栏：日期 → 时间 → 城市行（IP 与城市互斥占同一格，切换不跳动）
     *   右栏：32×32 图标 / 温度 / 天气，始终显示（缺省位置也能取到天气）
     *   底部：三天预报只在已配置位置时显示 —— 没配置时不拿远处默认位置的
     *        预报误导用户，这一行整体隐藏。
     */
    const bool show_ip = AlphapiWeatherIpVisible();

    SetLabel(g_city, g_cache.city, sizeof(g_cache.city), AlphapiWeatherCity());
    {
        char ip[16] = {};
        if (!AlphapiWeatherIp(ip, sizeof(ip))) {
            std::snprintf(ip, sizeof(ip), "%s", "--");
        }
        SetLabel(g_ip, g_cache.ip, sizeof(g_cache.ip), ip);
    }

    const int temp_c = AlphapiWeatherTempC();
    if (temp_c == INT_MIN) {
        SetLabel(g_temp, g_cache.temp, sizeof(g_cache.temp), "--°C");
    } else {
        std::snprintf(buf, sizeof(buf), "%d°C", temp_c);
        SetLabel(g_temp, g_cache.temp, sizeof(g_cache.temp), buf);
    }

    /*
     * 天气描述：右栏 50px 宽 + 文字居中，单字（晴/阴）与两字（多云/阵雨）的
     * 视觉重心都落在栏中心 x=131，三字（雷阵雨）也自动居中不越界 ——
     * 不需要像 C 版那样按字数控 x 偏移。
     */
    SetLabel(g_desc, g_cache.desc, sizeof(g_cache.desc), WeatherCn(AlphapiWeatherCode()));

    g_icon = alphapi_weather_icon::Render(g_root, AlphapiWeatherCode());

    /* 三天预报：迷你图标 + 日名 + 最高温。
     * "26°" 宽约 18px；"-10°" 约 23px 也放得下（32+23 < 56），但 ≤ -10°C 时
     * 去掉度数符号多留 4px 余量，避免顶到下一段图标。
     * 图标 x 只在温度文本真的变化时才重算（lv_obj_update_layout 会触发整棵
     * 对象树布局，每秒 3 次是以前最重的一项无谓开销）。 */
    for (int i = 0; i < kForecastSlots; ++i) {
        int code = -1;
        int tmax = INT_MIN;
        const bool has = AlphapiWeatherForecast(i, &code, &tmax);

        SetLabel(g_fc_day[i], g_cache.fc_day[i], sizeof(g_cache.fc_day[i]),
                 ForecastDayCn(i));

        char tbuf[24] = {};   // int 最长 11 字符 + "°" 2 字节，留足余量防 -Wformat-truncation
        if (!has) {
            std::snprintf(tbuf, sizeof(tbuf), "%s", "--");
        } else if (tmax <= -10) {
            std::snprintf(tbuf, sizeof(tbuf), "%d", tmax);
        } else {
            std::snprintf(tbuf, sizeof(tbuf), "%d°", tmax);
        }
        const bool temp_changed = SetLabel(g_fc_temp[i], g_cache.fc_temp[i],
                                           sizeof(g_cache.fc_temp[i]), tbuf);

        g_fc_icon[i] = alphapi_weather_icon::RenderMini(g_root, i, has ? code : -1);

        if (g_fc_icon[i] != nullptr && (temp_changed || g_cache.fc_icon_x[i] < 0)) {
            lv_obj_update_layout(g_fc_temp[i]);
            const int temp_w = (g_fc_temp[i] != nullptr) ? lv_obj_get_width(g_fc_temp[i]) : 0;
            const int x = 22 + i * kForecastPitch + temp_w + 1;
            if (x != g_cache.fc_icon_x[i]) {
                lv_obj_set_pos(g_fc_icon[i], x, 82);
                g_cache.fc_icon_x[i] = x;
            }
        }
    }

    Show(g_ip, show_ip);
    Show(g_city, !show_ip);
    Show(g_temp, true);
    Show(g_desc, true);
    Show(g_icon, true);
    for (int i = 0; i < kForecastSlots; ++i) {
        Show(g_fc_day[i], !show_ip);
        Show(g_fc_temp[i], !show_ip);
        Show(g_fc_icon[i], !show_ip);
    }
}

void SetVisible(bool visible) {
    if (g_root == nullptr || visible == g_visible) {
        return;
    }
    g_visible = visible;
    if (visible) {
        lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_root);
        UpdateText();
        /*
         * 顶部状态栏那 22px 不归时钟页绘制（g_root 从 y=22 开始），切进来时
         * 标脏整屏，让 top_bar_ 用当前主题色重铺。
         *
         * 这里以前还调了 lv_refr_now(nullptr) 强制立即刷新：那是 v13 排查
         * 状态栏色差时加的，真因后来查明是 assets 里 skin.dark 的 #1f2937
         * 覆盖主题色（v14 已修），这行早已没有作用。更要命的是它跑在 LVGL
         * timer 回调里 —— lv_refr_now 会重入显示刷新流程，与 esp_lvgl_port
         * 的 flush / mutex 撞车时可能死锁；LVGL 锁一旦不释放，Application
         * 主循环每秒的 UpdateStatusBar() 就会一直等锁，整机表现为「用一段
         * 时间有概率卡死」。只保留 invalidate，交给正常刷新周期，不重入。
         */
        lv_obj_invalidate(lv_screen_active());
    } else {
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void TickCb(lv_timer_t* /*timer*/) {
    const DeviceState state = Application::GetInstance().GetDeviceState();
    // 双固件布局下没有同进程内的电台页，收音机是 ota_1 里另一套固件
    SetVisible(state == kDeviceStateIdle);
    if (g_visible) {
        UpdateText();
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
    lv_obj_set_style_bg_color(g_root, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    g_visible = false;

    /* ---------- B 方案「左时右天 + 底部三天预报」 ---------- */
    /* 屏坐标 = 内容区坐标 + 22。下面注释里的屏幕位置以「墨迹」为准：
     * 日期 24..37 → 时间 47..70 → 城市 81..94 → 预报行 104..116，间隙均 10px。 */

    /* 左栏 · 日期（屏幕墨迹 24..37）。label 顶 y0 → 屏 22，与状态栏只隔 2px */
    g_date = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_date, &alphapi_cn_16, 0);
    lv_obj_set_style_text_color(g_date, lv_color_hex(kMuted), 0);
    lv_label_set_text(g_date, "--/-- 周-");
    lv_obj_align(g_date, LV_ALIGN_TOP_LEFT, 8, 0);

    /* 左栏 · 大字时间（屏幕墨迹 47..70）。32px 字体行高 24、数字墨迹占行内 1..24，
     * 所以 label 顶 y24 → 屏 46。左对齐 x8，与日期/城市同一锚点。
     * 不用 40px：左栏只有 96px 宽，40px 的「21:47」约 100px 会顶到 x104 的分栏线。 */
    g_time = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_time, &alphapi_time_32, 0);
    lv_obj_set_style_text_color(g_time, lv_color_hex(kAccent), 0);
    lv_label_set_text(g_time, "--:--");
    lv_obj_align(g_time, LV_ALIGN_TOP_LEFT, 8, 24);

    /* 左栏 · 城市（屏幕 81..94）与设备 IP 共用同一格，互斥显示：
     * 没设置过天气位置 → 显示 IP（暖黄 14px，浏览器打开它即可配置）
     * 设置过           → 显示城市名（次要灰 16px）
     * 两者同格，切换时这一行不会跳动。 */
    g_city = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_city, &alphapi_cn_16, 0);
    lv_obj_set_style_text_color(g_city, lv_color_hex(kMuted), 0);
    lv_label_set_text(g_city, "福州");
    lv_obj_align(g_city, LV_ALIGN_TOP_LEFT, 8, 57);

    g_ip = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_ip, lv_color_hex(kWarm), 0);
    lv_label_set_text(g_ip, "--");
    lv_obj_align(g_ip, LV_ALIGN_TOP_LEFT, 8, 59);

    /* 左右分栏竖线（屏幕 x104，y 26..94）：上端齐图标顶、下端齐右栏天气墨迹底 */
    lv_obj_t* line = lv_obj_create(g_root);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 1, 68);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x1E2A40), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 104, 4);

    /* 右栏（x 106..156，50px 宽，文字居中）：32×32 矢量图标由 weather_icon 模块
     * 定位在 (115,4)；温度墨迹 63..76、天气描述墨迹 81..94（与左栏城市同高）。
     *
     * 温度用 alphapi_temp_16（Segoe UI Bold 16px 子集：0-9 - ° C），不能用 cn_16 ——
     * cn_16 的中文子集只覆盖 ASCII + 139 个汉字，没有 U+00B0，° 会掉到 LVGL 默认
     * 字体上，描边和基线都和 simhei 的 C 对不上（实测反馈"主天气温度符号有问题"）。
     * temp_16 的 line_height=11 / base_line=0，字形墨迹从 label 顶部算起；
     * cn_16 是 line_height=19 / base_line=3，墨迹从行内 +4 才开始。
     * 所以 label y 取 43（= 39 + 4）才能让墨迹留在原来的 63..76，与图标/描述间距不变。 */
    g_temp = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_temp, &alphapi_temp_16, 0);
    lv_obj_set_style_text_color(g_temp, lv_color_hex(kWarm), 0);
    lv_obj_set_style_text_align(g_temp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_temp, 50);
    lv_label_set_text(g_temp, "--°C");
    lv_obj_align(g_temp, LV_ALIGN_TOP_LEFT, 106, 43);

    g_desc = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_desc, &alphapi_cn_16, 0);
    lv_obj_set_style_text_color(g_desc, lv_color_hex(kPrimary), 0);
    lv_obj_set_style_text_align(g_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_desc, 50);
    lv_label_set_text(g_desc, "--");
    lv_obj_align(g_desc, LV_ALIGN_TOP_LEFT, 106, 57);

    /* 底部三天预报行（屏幕 104..116）—— 顺序「日名 · 温度 · 图标」：
     * 段 i 起点 x = 8 + 50i；日名 @x8/58/108（cn_12，与日期同一个 x 锚点）；
     * 温度 @x22/72/122（num_12，比日名低 3px 让两组数字基线对齐）；
     * 迷你图标 12×12 紧跟温度右侧 1px —— x 在 UpdateText 里按温度实际宽度实时
     * 定位（温度最宽是 "-10"=20px：≤ -10°C 时已经去掉度数符号），
     * 图标最右到 55，段末 58，不会挤到下一段。 */
    for (int i = 0; i < kForecastSlots; ++i) {
        g_fc_day[i] = lv_label_create(g_root);
        lv_obj_set_style_text_font(g_fc_day[i], &alphapi_cn_12, 0);
        lv_obj_set_style_text_color(g_fc_day[i], lv_color_hex(kMuted), 0);
        lv_label_set_text(g_fc_day[i], ForecastDayCn(i));
        lv_obj_align(g_fc_day[i], LV_ALIGN_TOP_LEFT, 8 + i * kForecastPitch, 82);

        g_fc_temp[i] = lv_label_create(g_root);
        lv_obj_set_style_text_font(g_fc_temp[i], &alphapi_num_12, 0);
        lv_obj_set_style_text_color(g_fc_temp[i], lv_color_hex(kForecastTemp), 0);
        lv_label_set_text(g_fc_temp[i], "--");
        lv_obj_align(g_fc_temp[i], LV_ALIGN_TOP_LEFT, 22 + i * kForecastPitch, 85);
    }

    UpdateText();
    ESP_LOGI(kTag, "idle clock UI created (layout B, 32px time + 32px icon + forecast)");
}

/*
 * 开机引导：独立 FreeRTOS 任务，不再挂 ESP_TIMER_TASK。
 * ---------------------------------------------------------------------------
 * 原来 SNTP 初始化和 BuildUi 都放在 esp_timer 回调里。esp_timer 任务栈只有
 * CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584 B，而 BuildUi 要创建 40+ 个 LVGL
 * 对象、递归画 4 组矢量图标；DisplayLockGuard 又要等 LVGL mutex（最长 30 s），
 * 一旦阻塞就把整个 esp_timer 任务挂住，系统所有软件定时器停摆。栈溢出还会
 * 悄悄踩坏相邻内存，之后随机发作 —— 正是「用一段时间」才复现的典型形态。
 * 改成独立 4096 B 任务，栈和互斥两个隐患一起消掉。
 *
 * 注意 DisplayLockGuard 必须放在内层块里：vTaskDelete(nullptr) 不返回，
 * 若析构排在其后，LVGL 锁就永远解不开。
 */
void BootTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));   // 板卡构造阶段 lwIP 未就绪，晚点起 SNTP
    StartSntpOnce();
    vTaskDelay(pdMS_TO_TICKS(1000));

    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "no display, idle clock skipped");
        vTaskDelete(nullptr);
        return;
    }
    {
        DisplayLockGuard lock(display);
        if (lock) {
            BuildUi(lv_screen_active());
            if (g_root != nullptr) {
                lv_timer_create(TickCb, 1000, nullptr);
            }
        } else {
            ESP_LOGW(kTag, "display lock failed, idle clock skipped");
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace

void AlphapiEnsureValidTime(void) {
    // 先设时区，让后面的时间显示与 SNTP/时钟页走同一套 CST-8 规则。
    EnsureTimezone();

    char mon[4] = {};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(kBuildDate, "%3s %d %d", mon, &day, &year) != 3 ||
        std::sscanf(kBuildTime, "%d:%d:%d", &hh, &mm, &ss) != 3) {
        ESP_LOGW(kTag, "cannot parse build time (%s %s)", kBuildDate, kBuildTime);
        return;
    }
    const int month = MonthIndex(mon);
    if (month < 0 || year < 2024) {
        ESP_LOGW(kTag, "build date not usable: %s", kBuildDate);
        return;
    }

    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = ss;
    t.tm_isdst = 0;
    const time_t build_utc = timegm(&t) - kBuildTzOffset;
    if (build_utc <= 0) {
        ESP_LOGW(kTag, "timegm failed for %s %s", kBuildDate, kBuildTime);
        return;
    }

    /*
     * 只有「与编译时刻相差 7 天以内」的时间才保留（通常来自 SNTP、HTTP Date
     * 或软复位后 RTC 里留下的真实时间）。1970、1881，以及任何看起来像 2025 年
     * 但仍早于证书生效日的旧时间，一律覆盖 —— 只按「年份 >= 2020」判断会漏掉后者。
     */
    const time_t now = time(nullptr);
    constexpr time_t kTolerance = 7 * 24 * 3600;
    if (now >= build_utc - kTolerance && now <= build_utc + kTolerance) {
        ESP_LOGI(kTag, "system time already within 7d of build time, keep it");
        return;
    }

    struct timeval tv = {build_utc, 0};
    settimeofday(&tv, nullptr);
    ESP_LOGW(kTag,
             "system time seeded from build time %s %s (UTC+8); "
             "SNTP/HTTP Date may refine it later",
             kBuildDate, kBuildTime);
}

void AlphapiSntpKick(void) {
    StartSntpOnce();
    esp_sntp_restart();
}

void AlphapiIdleClockStart(void) {
    // 校时 + 建 UI 全在独立任务里延时完成，不再占用 esp_timer 任务的小栈。
    if (xTaskCreate(BootTask, "idle_boot", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "idle clock boot task create failed");
    }
}
