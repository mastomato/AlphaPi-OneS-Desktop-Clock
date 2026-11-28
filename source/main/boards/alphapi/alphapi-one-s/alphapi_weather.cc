#include "alphapi_weather.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>
#include <sys/time.h>

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "cJSON.h"
#include "settings.h"
#include "ssid_manager.h"
#include "wifi_board.h"
#include "wifi_manager.h"

// Existing audio counters are sampled only; the audio path stays unchanged.
extern volatile unsigned g_alphapi_dbg_rad;
extern volatile unsigned g_alphapi_dbg_eq;
extern volatile unsigned g_alphapi_dbg_enc_ok;
extern volatile unsigned g_alphapi_dbg_enc_err;
extern volatile unsigned g_alphapi_dbg_sq;
extern volatile unsigned g_alphapi_dbg_drop;
extern volatile unsigned g_alphapi_mic_timeout;

namespace {

constexpr char kTag[] = "Weather";

// 15 分钟一次：天气变化慢，没必要频繁请求；失败也不影响其它功能。
constexpr int kRefreshSeconds = 15 * 60;
constexpr int kFirstDelaySeconds = 3;    // 尽早拿到 Date 头，把编译时间基准校准成真实时间
constexpr int kRetrySeconds = 6;         // 首次常赶在 Wi-Fi 就绪前，短间隔重试而不是等 15 分钟

constexpr char kNvsNamespace[] = "weather";
constexpr char kDefaultCity[] = "福州";

/* ------------------------------------------------------------------------- *
 * 位置：NVS 持久化 + 双缓冲无锁读取
 * ------------------------------------------------------------------------- */
struct CityPreset {
    const char* name;
    const char* lat;
    const char* lon;
};

// 常用城市预设。经纬度用字符串存，拼 open-meteo URL 时直接套用，避免浮点格式化误差。
constexpr CityPreset kCities[] = {
    {"北京", "39.9042", "116.4074"},   {"上海", "31.2304", "121.4737"},
    {"广州", "23.1291", "113.2644"},   {"深圳", "22.5431", "114.0579"},
    {"杭州", "30.2741", "120.1551"},   {"南京", "32.0603", "118.7969"},
    {"成都", "30.5728", "104.0668"},   {"重庆", "29.5630", "106.5516"},
    {"武汉", "30.5928", "114.3055"},   {"西安", "34.3416", "108.9398"},
    {"苏州", "31.2989", "120.5853"},   {"天津", "39.3434", "117.3616"},
    {"长沙", "28.2282", "112.9388"},   {"郑州", "34.7466", "113.6254"},
    {"青岛", "36.0671", "120.3826"},   {"大连", "38.9140", "121.6147"},
    {"厦门", "24.4798", "118.0894"},   {"福州", "26.0745", "119.2965"},
    {"昆明", "25.0389", "102.7183"},   {"贵阳", "26.6470", "106.6302"},
    {"南宁", "22.8170", "108.3665"},   {"海口", "20.0444", "110.1999"},
    {"合肥", "31.8206", "117.2272"},   {"济南", "36.6512", "117.1201"},
    {"哈尔滨", "45.8038", "126.5349"}, {"长春", "43.8171", "125.3235"},
    {"沈阳", "41.8057", "123.4315"},   {"石家庄", "38.0428", "114.5149"},
    {"太原", "37.8706", "112.5489"},   {"呼和浩特", "40.8414", "111.7519"},
    {"乌鲁木齐", "43.8256", "87.6168"}, {"兰州", "36.0611", "103.8343"},
    {"西宁", "36.6171", "101.7782"},   {"银川", "38.4872", "106.2309"},
    {"拉萨", "29.6525", "91.1721"},    {"南昌", "28.6820", "115.8579"},
    {"无锡", "31.4912", "120.3119"},   {"宁波", "29.8683", "121.5440"},
    {"温州", "27.9938", "120.6994"},   {"佛山", "23.0218", "113.1219"},
    {"东莞", "23.0207", "113.7518"},   {"珠海", "22.2710", "113.5767"},
    {"泉州", "24.8741", "118.6757"},   {"桂林", "25.2736", "110.2994"},
    {"洛阳", "34.6197", "112.4539"},   {"徐州", "34.2058", "117.2848"},
    {"烟台", "37.4638", "121.4479"},
};
constexpr int kCityCount = static_cast<int>(sizeof(kCities) / sizeof(kCities[0]));

struct Location {
    char lat[16];
    char lon[16];
    char city[32];
};

Location g_loc[2] = {};
volatile int g_loc_idx = 0;             // 双缓冲：写者填另一半后再切索引，读者永远拿到完整结构
TaskHandle_t g_weather_task = nullptr;
httpd_handle_t g_httpd = nullptr;

char g_text[24] = "TEMP --C";
bool g_valid = false;
int g_code = -1;
int g_temp = INT_MIN;

/* 未来三天预报（今天起 3 天）：weather_code + 最高温。
 * 与 current 一次请求拿到，解析后存在这里，时钟页每秒来读。 */
constexpr int kForecastDays = 3;
bool g_fc_valid[kForecastDays] = {};
int g_fc_code[kForecastDays] = {-1, -1, -1};
int g_fc_tmax[kForecastDays] = {INT_MIN, INT_MIN, INT_MIN};

volatile bool g_configured = false;      // NVS 里已保存过位置
int64_t g_ip_reveal_until_us = 0;       // /showip 临时显示窗口（esp_timer 单调时钟）

Location Snapshot() {
    const int i = g_loc_idx;
    return g_loc[i];
}

bool NormalizeCoord(const char* in, char* out, size_t n, double lo, double hi) {
    char* end = nullptr;
    const double v = std::strtod(in, &end);
    if (end == in || v < lo || v > hi) {
        return false;
    }
    std::snprintf(out, n, "%.4f", v);
    return true;
}

// 屏幕字库只含有限汉字；这里过滤掉会破坏 HTML 的字符，长度也做限制。
void SanitizeCity(const char* in, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 || c == '<' || c == '>' || c == '"' || c == '\'' || c == '&') {
            continue;
        }
        out[j++] = static_cast<char>(c);
    }
    out[j] = '\0';
    if (j == 0) {
        std::snprintf(out, n, "自定义");
    }
}

bool SaveLocation(const char* lat, const char* lon, const char* city) {
    char la[16], lo[16], ci[32];
    if (!NormalizeCoord(lat, la, sizeof(la), -90.0, 90.0) ||
        !NormalizeCoord(lon, lo, sizeof(lo), -180.0, 180.0)) {
        return false;
    }
    SanitizeCity(city, ci, sizeof(ci));

    {
        Settings s(kNvsNamespace, true);   // 析构时 nvs_commit
        s.SetString("lat", la);
        s.SetString("lon", lo);
        s.SetString("city", ci);
    }

    Location next = {};
    std::memcpy(next.lat, la, sizeof(next.lat));
    std::memcpy(next.lon, lo, sizeof(next.lon));
    std::memcpy(next.city, ci, sizeof(next.city));
    const int target = 1 - g_loc_idx;
    g_loc[target] = next;
    g_loc_idx = target;
    g_configured = true;   // 屏幕上的 IP 提示会在下一次刷新时消失

    ESP_LOGI(kTag, "location saved: %s (%s, %s), hide IP hint", ci, la, lo);
    if (g_weather_task != nullptr) {
        xTaskNotifyGive(g_weather_task);   // 打断 15 分钟等待，立刻重拉天气
    }
    return true;
}

void LoadLocation() {
    char def_lat[16], def_lon[16];
    std::snprintf(def_lat, sizeof(def_lat), "%.4f", static_cast<double>(ALPHAPI_WEATHER_LAT));
    std::snprintf(def_lon, sizeof(def_lon), "%.4f", static_cast<double>(ALPHAPI_WEATHER_LON));

    Location default_loc = {};
    std::memcpy(default_loc.lat, def_lat, sizeof(default_loc.lat));
    std::memcpy(default_loc.lon, def_lon, sizeof(default_loc.lon));
    std::snprintf(default_loc.city, sizeof(default_loc.city), "%s", kDefaultCity);
    g_loc[0] = default_loc;
    g_loc[1] = default_loc;
    g_loc_idx = 0;

    Settings s(kNvsNamespace, true);
    // 缺省值用空串，才能区分「从没配置过」和「配置成了缺省的福州」。
    const std::string lat = s.GetString("lat", "");
    const std::string lon = s.GetString("lon", "");
    const std::string city = s.GetString("city", kDefaultCity);
    if (lat.empty() || lon.empty()) {
        g_configured = false;
        ESP_LOGI(kTag, "no saved location yet, keep IP hint visible");
        return;
    }
    char la[16], lo[16], ci[32];
    if (!NormalizeCoord(lat.c_str(), la, sizeof(la), -90.0, 90.0) ||
        !NormalizeCoord(lon.c_str(), lo, sizeof(lo), -180.0, 180.0)) {
        ESP_LOGW(kTag, "invalid saved location, using default");
        g_configured = false;
        return;
    }
    SanitizeCity(city.c_str(), ci, sizeof(ci));
    std::memcpy(g_loc[0].lat, la, sizeof(la));
    std::memcpy(g_loc[0].lon, lo, sizeof(lo));
    std::memcpy(g_loc[0].city, ci, sizeof(ci));
    g_loc[1] = g_loc[0];
    g_configured = true;
    ESP_LOGI(kTag, "location loaded: %s (%s, %s), IP hint hidden", ci, la, lo);
}

/* ------------------------------------------------------------------------- *
 * 天气拉取 / HTTP Date 校时（沿用原有逻辑）
 * ------------------------------------------------------------------------- */
const char* CodeText(int code) {
    if (code == 0) return "SUN";
    if (code <= 2) return "PTCLDY";
    if (code == 3) return "CLOUD";
    if (code <= 48) return "FOG";
    if (code <= 57) return "DRIZZLE";
    if (code <= 67) return "RAIN";
    if (code <= 77) return "SNOW";
    if (code <= 82) return "SHOWER";
    if (code <= 86) return "SNOW";
    return "STORM";
}

void ParsePayload(const char* json, int len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) {
        ESP_LOGW(kTag, "json parse failed");
        return;
    }
    const cJSON* current = cJSON_GetObjectItem(root, "current");
    if (current == nullptr) {
        ESP_LOGW(kTag, "no 'current' object");
        cJSON_Delete(root);
        return;
    }
    const cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
    const cJSON* code = cJSON_GetObjectItem(current, "weather_code");
    if (cJSON_IsNumber(temp)) {
        const int t = static_cast<int>(temp->valuedouble + (temp->valuedouble >= 0 ? 0.5 : -0.5));
        const int wmo = cJSON_IsNumber(code) ? code->valueint : -1;
        if (wmo >= 0) {
            std::snprintf(g_text, sizeof(g_text), "TEMP %dC %s", t, CodeText(wmo));
        } else {
            std::snprintf(g_text, sizeof(g_text), "TEMP %dC", t);
        }
        g_code = wmo;
        g_temp = t;
        g_valid = true;
        ESP_LOGI(kTag, "%s", g_text);
    }

    /* daily：未来三天（含今天）的天气码与最高温，时钟页底部预报行用。
     * 数组长度不足 3 时只更新拿到的前几天，缺的天保持旧值。 */
    const cJSON* daily = cJSON_GetObjectItem(root, "daily");
    if (daily != nullptr) {
        const cJSON* codes = cJSON_GetObjectItem(daily, "weather_code");
        const cJSON* tmaxs = cJSON_GetObjectItem(daily, "temperature_2m_max");
        if (cJSON_IsArray(codes) && cJSON_IsArray(tmaxs)) {
            const int n = std::min(kForecastDays, static_cast<int>(cJSON_GetArraySize(codes)));
            for (int i = 0; i < kForecastDays; ++i) {
                if (i >= n) {
                    break;
                }
                const cJSON* c = cJSON_GetArrayItem(codes, i);
                const cJSON* tm = cJSON_GetArrayItem(tmaxs, i);
                if (!cJSON_IsNumber(c) || !cJSON_IsNumber(tm)) {
                    continue;
                }
                const double v = tm->valuedouble;
                g_fc_code[i] = c->valueint;
                g_fc_tmax[i] = static_cast<int>(v + (v >= 0 ? 0.5 : -0.5));
                g_fc_valid[i] = true;
            }
            ESP_LOGI(kTag, "daily: d0=%d/%dC d1=%d/%dC d2=%d/%dC",
                     g_fc_code[0], g_fc_tmax[0], g_fc_code[1], g_fc_tmax[1],
                     g_fc_code[2], g_fc_tmax[2]);
        }
    }
    cJSON_Delete(root);
}

bool g_time_fixed = false;

void SetTimeFromHttpDate(const char* date) {
    if (date == nullptr) {
        return;
    }
    // 不要用「年份 >= 2024」判断时间是否已经对：app_main 用编译时刻设的基准时间
    // 会被误判成「已校准」，真实时间就永远写不进来。
    static const char* kMon[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    const char* comma = std::strchr(date, ',');
    if (comma == nullptr) {
        return;
    }
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon[4] = {0};
    if (std::sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hour, &min, &sec) != 6) {
        return;
    }
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(mon, kMon[i], 3) == 0) {
            month = i;
            break;
        }
    }
    if (month < 0 || year < 2024) {
        return;
    }
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    const time_t utc = timegm(&t);
    if (utc <= 0) {
        return;
    }
    // 每 15 分钟校准一次：HTTP Date 是权威 GMT，能纠正任何来源写偏的时间。
    const time_t drift = time(nullptr) - utc;
    if (drift > -60 && drift < 60) {
        g_time_fixed = true;
        return;
    }
    struct timeval tv = {utc, 0};
    settimeofday(&tv, nullptr);
    g_time_fixed = true;
    ESP_LOGI(kTag, "system time set from HTTP Date header: %04d-%02d-%02d %02d:%02d UTC (drift %ld s)",
             year, month + 1, day, hour, min, static_cast<long>(drift));
}

esp_err_t OnHttpEvent(esp_http_client_event_t* ev) {
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key != nullptr) {
        if (strcasecmp(ev->header_key, "Date") == 0) {
            SetTimeFromHttpDate(ev->header_value);
        }
    }
    return ESP_OK;
}

bool FetchOnce() {
    const Location loc = Snapshot();
    char url[224];
    std::snprintf(url, sizeof(url),
                  "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                  "&current=temperature_2m,weather_code"
                  "&daily=weather_code,temperature_2m_max&forecast_days=3",
                  loc.lat, loc.lon);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 512;
    cfg.disable_auto_redirect = false;
    cfg.event_handler = OnHttpEvent;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    // 带上 daily 三天后响应约 600~700 B，1024 B 缓冲留足余量。
    static char body[1024];
    int total = 0;
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        while (total < static_cast<int>(sizeof(body)) - 1) {
            const int read = esp_http_client_read(client, body + total,
                                                  sizeof(body) - 1 - total);
            if (read <= 0) {
                break;
            }
            total += read;
        }
        body[total] = '\0';
        ok = total > 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        ESP_LOGW(kTag, "http fetch failed (%s)", url);
        return false;
    }
    ParsePayload(body, total);
    return g_valid || g_time_fixed;
}

void WeatherTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(kFirstDelaySeconds * 1000));
    for (;;) {
        const bool ok = FetchOnce();
        // 保存新位置会 xTaskNotifyGive 打断等待，立刻重拉，不用等 15 分钟。
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((ok ? kRefreshSeconds : kRetrySeconds) * 1000));
    }
}

/* ------------------------------------------------------------------------- *
 * 统一设置页：Wi-Fi 配网（最上面） + 天气位置（下面），全设备只有这一个 web 页
 * ---------------------------------------------------------------------------
 * 原来是两个互相独立的页面：配网 AP 的 http://192.168.4.1/ 和天气设置页
 * http://<设备IP>/，两套 httpd 都抢 80 端口。现在合并：
 *   - 本文件在开机后起唯一的 httpd，绑 0.0.0.0:80，STA 与 AP 模式都能访问；
 *   - WifiConfigurationAp 只保留 AP / DNS / 周期扫描，不再起自己的 web server；
 *   - 页面顶部是 Wi-Fi 配网（状态、地址、扫描、SSID/密码、进出配网模式），
 *     下面才是原来的天气位置设置。
 * 这样既满足「一个页面」，也顺手消掉了 80 端口抢占这个间歇性卡死诱因。
 * ------------------------------------------------------------------------- */
bool GetStationIp(char* buf, size_t len) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return false;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    const uint32_t a = info.ip.addr;   // 网络字节序，小端机上低字节是第一段
    std::snprintf(buf, len, "%u.%u.%u.%u",
                  static_cast<unsigned>(a & 0xff), static_cast<unsigned>((a >> 8) & 0xff),
                  static_cast<unsigned>((a >> 16) & 0xff), static_cast<unsigned>((a >> 24) & 0xff));
    return true;
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void UrlDecode(const char* src, size_t len, char* dst, size_t dst_n) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < dst_n; ++i) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < len) {
            const int hi = HexNibble(src[i + 1]);
            const int lo = HexNibble(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = static_cast<char>((hi << 4) | lo);
                i += 2;
            }
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

// 从 application/x-www-form-urlencoded 的 body 里取一个字段
bool FormValue(const char* body, const char* key, char* out, size_t out_n) {
    const size_t key_len = std::strlen(key);
    const char* p = body;
    while (p != nullptr && *p != '\0') {
        if (std::strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char* v = p + key_len + 1;
            const char* end = std::strchr(v, '&');
            UrlDecode(v, end == nullptr ? std::strlen(v) : static_cast<size_t>(end - v), out, out_n);
            return true;
        }
        p = std::strchr(p, '&');
        if (p != nullptr) ++p;
    }
    out[0] = '\0';
    return false;
}

// SSID 是用户可控输入，插进 HTML / JSON 前必须转义，否则会破坏页面结构。
void HtmlEscape(const char* in, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 6 < n; ++i) {
        const char c = in[i];
        const char* rep = nullptr;
        if (c == '&') rep = "&amp;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == '"') rep = "&quot;";
        else if (c == '\'') rep = "&#39;";
        if (rep != nullptr) {
            while (*rep != '\0' && j + 1 < n) out[j++] = *rep++;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

void JsonEscape(const char* in, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 6 < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = static_cast<char>(c);
        } else if (c < 0x20) {
            std::snprintf(out + j, n - j, "\\u%04x", c);
            j += 6;
        } else {
            out[j++] = static_cast<char>(c);
        }
    }
    out[j] = '\0';
}

// 可选功能：按公网出口 IP 自动定位（明文 HTTP，失败时页面提示手动选城市）
bool GeoLocate(char* city, size_t city_n, char* lat, size_t lat_n, char* lon, size_t lon_n) {
    esp_http_client_config_t cfg = {};
    cfg.url = "http://ip-api.com/json/?fields=status,message,city,lat,lon";
    cfg.timeout_ms = 7000;
    cfg.buffer_size = 512;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    static char body[512];
    int total = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        while (total < static_cast<int>(sizeof(body)) - 1) {
            const int read = esp_http_client_read(client, body + total,
                                                  sizeof(body) - 1 - total);
            if (read <= 0) break;
            total += read;
        }
        body[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (total <= 0) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body, total);
    if (root == nullptr) {
        return false;
    }
    const cJSON* status = cJSON_GetObjectItem(root, "status");
    const cJSON* jlat = cJSON_GetObjectItem(root, "lat");
    const cJSON* jlon = cJSON_GetObjectItem(root, "lon");
    const cJSON* jcity = cJSON_GetObjectItem(root, "city");
    bool ok = false;
    if (cJSON_IsString(status) && std::strcmp(status->valuestring, "success") == 0 &&
        cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
        std::snprintf(lat, lat_n, "%.4f", jlat->valuedouble);
        std::snprintf(lon, lon_n, "%.4f", jlon->valuedouble);
        std::snprintf(city, city_n, "%s",
                      cJSON_IsString(jcity) ? jcity->valuestring : "IP定位");
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

const char kHtmlHead[] = R"HTML(<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AlphaPi One S 设置</title><style>
body{font-family:system-ui,-apple-system,"Microsoft YaHei",sans-serif;background:#0E1420;color:#E6EEF7;margin:0 auto;padding:18px;max-width:520px}
h1{font-size:20px;color:#8FE3F0;margin:0 0 4px}
h2{font-size:16px;color:#FFD27A;margin:0 0 8px}
.small{color:#7186A0;font-size:13px;line-height:1.7}
.card{background:#16203A;border-radius:12px;padding:16px;margin:14px 0}
label{display:block;margin:12px 0 5px;font-size:14px}
input,select{width:100%;box-sizing:border-box;padding:9px;border:1px solid #34465F;border-radius:8px;background:#0E1420;color:#E6EEF7;font-size:15px}
button,.btn{display:inline-block;margin-top:16px;padding:10px 18px;border:0;border-radius:8px;background:#8FE3F0;color:#0E1420;font-size:15px;font-weight:600;cursor:pointer;text-decoration:none}
.btn2{background:#243352;color:#8FE3F0;margin-left:8px;margin-top:10px}
code{color:#FFD27A}.err{color:#FF9A8B}.ok{color:#9BE59B}
.aps{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.ap{padding:6px 10px;border:1px solid #34465F;border-radius:7px;background:#0E1420;color:#B8C4D6;font-size:13px;cursor:pointer}
.ap:hover{border-color:#8FE3F0;color:#8FE3F0}
.pill{display:inline-block;padding:2px 8px;border-radius:6px;background:#243352;color:#8FE3F0;font-size:12px}
</style></head><body>
<h1>AlphaPi One S 设置</h1>
<div class="small">Wi-Fi 配网 · 天气位置 · 网络收音机 —— 全部在这一个页面；配网只需一次，小智与收音机共用，重启后仍然生效</div>
)HTML";

const char kHtmlTail[] = R"HTML(<div class="small">
<p><b>配网说明</b>：正常联网时直接在本页改 Wi-Fi；进配网模式后，手机/电脑连上设备热点，
浏览器打开 <code>http://192.168.4.1/</code>，看到的仍是本页。
也可以在设备上长按 A 键 3 秒进 / 出配网模式。</p>
<p><b>天气说明</b>：保存位置后会立即重新拉取天气，不需要重启；屏幕右上角的
IP 提示会自动消失。以后忘了设备 IP，可点下面按钮让屏幕临时再显示 60 秒：<br>
<a class="btn btn2" href="/showip">在屏幕上重新显示 IP</a></p>
<p>数据源 open-meteo.com（免密钥）；「按公网 IP 自动定位」用 ip-api.com，
国内网络可能不可用，失败时手动选城市即可。</p>
</div><script>
function pick(s){var o=s.options[s.selectedIndex];if(!o||!o.dataset.lat)return;
document.getElementById('lat').value=o.dataset.lat;
document.getElementById('lon').value=o.dataset.lon;
document.getElementById('label').value=o.dataset.name;}
function fillAp(s){document.getElementById('ssid').value=s;}
fetch('/wifi/scan').then(function(r){return r.json()}).then(function(d){
 var box=document.getElementById('aps'); if(!box)return;
 var list=(d&&d.aps)||[];
 if(!list.length){box.textContent='未扫描到网络，可手动输入 SSID';return;}
 box.innerHTML='';
 list.forEach(function(a){var b=document.createElement('button');b.type='button';b.className='ap';
 b.textContent=a.ssid+' ('+a.rssi+'dBm)';b.onclick=function(){fillAp(a.ssid)};box.appendChild(b);});
}).catch(function(){var box=document.getElementById('aps');
 if(box)box.textContent='扫描暂不可用，可手动输入 SSID';});
</script></body></html>)HTML";

esp_err_t RootHandler(httpd_req_t* req) {
    const Location loc = Snapshot();
    char ip[16] = "0.0.0.0";
    const bool has_ip = GetStationIp(ip, sizeof(ip));
    // 配网卡片含 SSID / 热点名，天气表单含中文说明；留足余量避免 -Wformat-truncation
    char buf[1024];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, kHtmlHead);

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[8] = {0};
        if (httpd_query_key_value(query, "err", v, sizeof(v)) == ESP_OK) {
            httpd_resp_sendstr_chunk(req, "<div class=\"card err\">保存失败：请检查经纬度，或自动定位服务不可用。</div>");
        } else if (httpd_query_key_value(query, "ok", v, sizeof(v)) == ESP_OK) {
            httpd_resp_sendstr_chunk(req, "<div class=\"card ok\">已保存，天气正在刷新。</div>");
        } else if (httpd_query_key_value(query, "werr", v, sizeof(v)) == ESP_OK) {
            httpd_resp_sendstr_chunk(req, "<div class=\"card err\">Wi-Fi 保存失败：SSID 不能为空。</div>");
        } else if (httpd_query_key_value(query, "wok", v, sizeof(v)) == ESP_OK) {
            httpd_resp_sendstr_chunk(req, "<div class=\"card ok\">Wi-Fi 已保存，正在重新连接；连上后用新地址打开本页。</div>");
        }
    }

    /* ===== ① Wi-Fi 配网（用户要求放最上面） ===== */
    auto& wifi = WifiManager::GetInstance();
    const bool ap_mode = wifi.IsConfigMode();
    const std::string cur_ssid = wifi.GetSsid();
    const std::string ap_ssid = ap_mode ? wifi.GetApSsid() : std::string();
    char esc_cur[96] = {}, esc_ap[96] = {};
    HtmlEscape(cur_ssid.c_str(), esc_cur, sizeof(esc_cur));
    HtmlEscape(ap_ssid.c_str(), esc_ap, sizeof(esc_ap));

    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h2>① Wi-Fi 配网</h2>");
    if (ap_mode) {
        std::snprintf(buf, sizeof(buf),
                      "<div class=\"small\"><span class=\"pill\">配网模式</span> 设备热点 <b>%s</b><br>"
                      "配网地址 <code>http://192.168.4.1/</code> —— 你正在看的这个页面</div>",
                      esc_ap);
    } else if (has_ip) {
        std::snprintf(buf, sizeof(buf),
                      "<div class=\"small\"><span class=\"pill\">已联网</span> 当前网络 <b>%s</b><br>"
                      "设备地址 <code>http://%s/</code> · 配网模式地址 <code>http://192.168.4.1/</code></div>",
                      esc_cur[0] != '\0' ? esc_cur : "（未知）", ip);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "<div class=\"small\"><span class=\"pill\">未联网</span> 设备尚未取得 IP，"
                      "可直接填下面的 Wi-Fi 信息，或在设备上长按 A 键 3 秒进配网模式<br>"
                      "配网地址 <code>http://192.168.4.1/</code></div>");
    }
    httpd_resp_sendstr_chunk(req, buf);

    const auto saved_networks = SsidManager::GetInstance().GetSsidList();
    if (!saved_networks.empty()) {
        httpd_resp_sendstr_chunk(req,
            "<form method=\"post\" action=\"/wifi/save\">"
            "<input type=\"hidden\" name=\"reuse\" value=\"1\">"
            "<label>已保存网络（最多 10 个，切换无需重新输入密码）</label>"
            "<select name=\"ssid\">");
        for (const auto& item : saved_networks) {
            char escaped[200] = {};
            HtmlEscape(item.ssid.c_str(), escaped, sizeof(escaped));
            std::snprintf(buf, sizeof(buf), "<option value=\"%s\">%s</option>",
                          escaped, escaped);
            httpd_resp_sendstr_chunk(req, buf);
        }
        httpd_resp_sendstr_chunk(req,
            "</select><button type=\"submit\">连接已保存网络</button></form>"
            "<div class=\"small\">下面可添加新网络或更新密码；其它网络记录会保留。"
            "列表满后，新网络替换列表末尾的记录。</div>");
    }

    httpd_resp_sendstr_chunk(req,
        "<div class=\"small\" style=\"margin-top:10px\">附近网络（点击填入 SSID）</div>"
        "<div class=\"aps\" id=\"aps\">正在扫描…</div>"
        "<form method=\"post\" action=\"/wifi/save\">"
        "<label>Wi-Fi 名称 SSID</label><input name=\"ssid\" id=\"ssid\" maxlength=\"32\" required "
        "placeholder=\"选择上方网络或手动输入\">"
        "<label>密码（开放网络留空）</label>"
        "<input name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"new-password\">"
        "<button type=\"submit\">保存并连接</button></form>");

    httpd_resp_sendstr_chunk(req, ap_mode
        ? "<a class=\"btn btn2\" href=\"/wifi/mode?enter=0\">退出配网模式并重新联网</a></div>"
        : "<a class=\"btn btn2\" href=\"/wifi/mode?enter=1\">进入配网模式（开热点）</a></div>");

    /* ===== ② 天气位置设置 ===== */
    std::snprintf(buf, sizeof(buf),
                  "<div class=\"card\"><h2>② 天气位置设置</h2>"
                  "<p>当前位置：<b>%s</b> &nbsp; 纬度 %s / 经度 %s</p>"
                  "<div class=\"small\">当前天气：%s</div></div>",
                  loc.city, loc.lat, loc.lon, g_valid ? g_text : "等待刷新");
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><form method=\"post\" action=\"/save\">"
                                  "<label>选择城市</label><select name=\"city\" id=\"city\" onchange=\"pick(this)\">");

    int selected = -1;
    for (int i = 0; i < kCityCount; ++i) {
        if (std::strcmp(loc.lat, kCities[i].lat) == 0 &&
            std::strcmp(loc.lon, kCities[i].lon) == 0) {
            selected = i;
            break;
        }
    }
    for (int i = 0; i < kCityCount; ++i) {
        std::snprintf(buf, sizeof(buf),
                      "<option value=\"%d\" data-lat=\"%s\" data-lon=\"%s\" data-name=\"%s\"%s>%s</option>",
                      i, kCities[i].lat, kCities[i].lon, kCities[i].name,
                      i == selected ? " selected" : "", kCities[i].name);
        httpd_resp_sendstr_chunk(req, buf);
    }
    std::snprintf(buf, sizeof(buf), "<option value=\"-1\"%s>自定义（填下面经纬度）</option></select>",
                  selected < 0 ? " selected" : "");
    httpd_resp_sendstr_chunk(req, buf);

    std::snprintf(buf, sizeof(buf),
                  "<label>纬度 latitude (-90 ~ 90)</label><input name=\"lat\" id=\"lat\" value=\"%s\">"
                  "<label>经度 longitude (-180 ~ 180)</label><input name=\"lon\" id=\"lon\" value=\"%s\">"
                  "<label>屏幕显示名称（建议用列表内城市，字库只含常用汉字）</label>"
                  "<input name=\"label\" id=\"label\" maxlength=\"24\" value=\"%s\">"
                  "<button type=\"submit\">保存并立即刷新天气</button></form>"
                  "<a class=\"btn btn2\" href=\"/geo\">按公网 IP 自动定位</a></div>",
                  loc.lat, loc.lon, loc.city);
    httpd_resp_sendstr_chunk(req, buf);

    /* ===== ③ 收音机（配网已共享；此处展示状态与后台入口） ===== */
    {
        // 小智配网时已把默认凭据同步到收音机的 alphapi_radio namespace，
        // 因此收音机槽开机即可联网，无需再配一次。这里读取该 namespace 让页面显示状态。
        nvs_handle_t rh;
        char rssid[33] = {0};
        size_t slen = sizeof(rssid);
        bool radio_wifi_ready = false;
        if (nvs_open("alphapi_radio", NVS_READONLY, &rh) == ESP_OK) {
            radio_wifi_ready = (nvs_get_str(rh, "ssid", rssid, &slen) == ESP_OK && rssid[0] != '\0');
            nvs_close(rh);
        }
        char resc[96] = {0};
        HtmlEscape(rssid, resc, sizeof(resc));
        std::snprintf(buf, sizeof(buf),
                      "<div class=\"card\"><h2>③ 网络收音机</h2>"
                      "<div class=\"small\">Wi-Fi 凭据已与小智共享：<b>%s</b><br>"
                      "在设备上<b>长按 D 键</b>切到收音机后即可直接联网播放，无需再次配网。"
                      "<br>电台列表、自定义电台地址等设置请在收音机页面内长按对应按键进入其后台。</div></div>",
                      radio_wifi_ready ? resc : "尚未同步（请先在上方保存一次 Wi-Fi）");
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, kHtmlTail);
    httpd_resp_sendstr_chunk(req, nullptr);   // 结束 chunked 响应
    return ESP_OK;
}

esp_err_t Redirect(httpd_req_t* req, const char* location) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

esp_err_t SaveHandler(httpd_req_t* req) {
    char body[512] = {0};
    int len = 0;
    while (len < static_cast<int>(sizeof(body)) - 1 && len < req->content_len) {
        const int want = static_cast<int>(sizeof(body)) - 1 - len;
        const int got = httpd_req_recv(req, body + len, want);
        if (got <= 0) break;
        len += got;
    }
    if (len <= 0) {
        return Redirect(req, "/?err=1");
    }
    body[len] = '\0';
    char sel[8] = {0}, lat[32] = {0}, lon[32] = {0}, label[64] = {0};
    FormValue(body, "city", sel, sizeof(sel));
    FormValue(body, "lat", lat, sizeof(lat));
    FormValue(body, "lon", lon, sizeof(lon));
    FormValue(body, "label", label, sizeof(label));

    bool ok = false;
    const int idx = std::atoi(sel);
    if (idx >= 0 && idx < kCityCount) {
        const char* name = label[0] != '\0' ? label : kCities[idx].name;
        ok = SaveLocation(kCities[idx].lat, kCities[idx].lon, name);
    } else {
        ok = SaveLocation(lat, lon, label);
    }
    return Redirect(req, ok ? "/?ok=1" : "/?err=1");
}

esp_err_t GeoHandler(httpd_req_t* req) {
    char city[32] = {0}, lat[16] = {0}, lon[16] = {0};
    const bool ok = GeoLocate(city, sizeof(city), lat, sizeof(lat), lon, sizeof(lon)) &&
                    SaveLocation(lat, lon, city);
    ESP_LOGI(kTag, "geo locate %s: %s (%s, %s)", ok ? "ok" : "failed", city, lat, lon);
    return Redirect(req, ok ? "/?ok=1" : "/?err=1");
}

// 已经配置过、屏幕上不再显示 IP 时，用它临时把 IP 叫回来 60 秒。
esp_err_t ShowIpHandler(httpd_req_t* req) {
    g_ip_reveal_until_us = esp_timer_get_time() + 60LL * 1000 * 1000;
    ESP_LOGI(kTag, "reveal IP on screen for 60s");
    return Redirect(req, "/?ok=1");
}

/* ---------------- Wi-Fi 配网接口（统一页面用） ---------------- */

esp_err_t WifiScanHandler(httpd_req_t* req) {
    constexpr uint16_t kMaxAps = 16;
    wifi_ap_record_t aps[kMaxAps] = {};
    uint16_t count = kMaxAps;
    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&count, aps);
    }
    if (err != ESP_OK) {
        // AP 模式下组件自己也在周期扫描，撞车时给空列表，前端会提示手动输入
        ESP_LOGW(kTag, "wifi scan unavailable: %s", esp_err_to_name(err));
        count = 0;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr_chunk(req, "{\"aps\":[");
    bool first = true;
    for (uint16_t i = 0; i < count; ++i) {
        if (aps[i].ssid[0] == '\0') {
            continue;
        }
        char raw[34] = {};
        std::memcpy(raw, aps[i].ssid, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';   // 32 字节满的 SSID 不保证有 NUL
        char ssid[132] = {};
        JsonEscape(raw, ssid, sizeof(ssid));
        char item[192];
        std::snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                      first ? "" : ",", ssid, static_cast<int>(aps[i].rssi));
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

esp_err_t WifiSaveHandler(httpd_req_t* req) {
    char body[512] = {0};
    int len = 0;
    while (len < static_cast<int>(sizeof(body)) - 1 && len < req->content_len) {
        const int got = httpd_req_recv(req, body + len,
                                       static_cast<int>(sizeof(body)) - 1 - len);
        if (got <= 0) break;
        len += got;
    }
    body[len] = '\0';
    char ssid[64] = {}, password[128] = {};
    FormValue(body, "ssid", ssid, sizeof(ssid));
    FormValue(body, "password", password, sizeof(password));
    if (ssid[0] == '\0') {
        return Redirect(req, "/?werr=1");
    }
    auto& ssid_manager = SsidManager::GetInstance();
    char reuse[8] = {};
    FormValue(body, "reuse", reuse, sizeof(reuse));
    if (reuse[0] == '1') {
        const auto saved = ssid_manager.GetSsidList();
        int index = -1;
        for (size_t i = 0; i < saved.size(); ++i) {
            if (saved[i].ssid == ssid) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            return Redirect(req, "/?werr=1");
        }
        ssid_manager.SetDefaultSsid(index);
    } else {
        ssid_manager.AddSsid(ssid, password);
    }
    ESP_LOGI(kTag, "wifi credentials saved for '%s', reconnecting", ssid);

    // Wi-Fi 操作交给 Application 主循环，避免在 httpd 任务里持 wifi mutex 时
    // 触发事件回调重入。StopConfigAp() 会发 ConfigModeExit，WifiBoard 收到后
    // 自动 TryWifiConnect()；STA 模式则直接重启 station。
    Application::GetInstance().Schedule([]() {
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConfigMode()) {
            wifi.StopConfigAp();
        } else {
            wifi.StopStation();
            wifi.StartStation();
        }
    });
    return Redirect(req, "/?wok=1");
}

esp_err_t WifiModeHandler(httpd_req_t* req) {
    char query[32] = {}, v[8] = {};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "enter", v, sizeof(v));
    const bool enter = (v[0] == '1');
    Application::GetInstance().Schedule([enter]() {
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        if (enter) {
            // 走板卡标准入口：ResetProtocol + 延迟 1s + StartWifiConfigMode()
            auto* board = static_cast<WifiBoard*>(&Board::GetInstance());
            board->EnterWifiConfigMode();
        } else if (wifi.IsConfigMode()) {
            wifi.StopConfigAp();
            app.SetDeviceState(kDeviceStateIdle);
        }
    });
    return Redirect(req, "/?wok=1");
}

// AP 模式下 DNS 会把手机/系统的 captive portal 探测域名全指到本机，
// 这些 URI 没注册会 404、弹不出登录页，统一兜底重定向到设置页。
esp_err_t CaptiveHandler(httpd_req_t* req) {
    return Redirect(req, "/");
}

esp_err_t StatusHandler(httpd_req_t* req) {
    wifi_ap_record_t ap{};
    const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    char body[768];
    std::snprintf(body, sizeof(body),
        "{\"build\":\"wifi-history-diag-20261120\",\"state\":%d,"
        "\"uptime_s\":%lld,\"wifi_connected\":%s,\"rssi\":%d,"
        "\"heap\":%u,\"iram_free\":%u,\"iram_maxblk\":%u,"
        "\"audio\":{\"read_calls\":%u,\"encode_queue\":%u,\"encoded\":%u,"
        "\"encode_errors\":%u,\"send_queue\":%u,\"drops\":%u,\"mic_timeouts\":%u}}",
        static_cast<int>(Application::GetInstance().GetDeviceState()),
        static_cast<long long>(esp_timer_get_time() / 1000000),
        connected ? "true" : "false", connected ? static_cast<int>(ap.rssi) : 0,
        static_cast<unsigned>(esp_get_free_heap_size()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(g_alphapi_dbg_rad), static_cast<unsigned>(g_alphapi_dbg_eq),
        static_cast<unsigned>(g_alphapi_dbg_enc_ok), static_cast<unsigned>(g_alphapi_dbg_enc_err),
        static_cast<unsigned>(g_alphapi_dbg_sq), static_cast<unsigned>(g_alphapi_dbg_drop),
        static_cast<unsigned>(g_alphapi_mic_timeout));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

void WebTask(void*) {
    // 不再等 STA IP：首次开机没有凭据时设备会直接进 AP 配网模式，那个模式下
    // 没有 STA IP，但 192.168.4.1 也要能打开同一个页面。开机延后 3 秒等
    // Wi-Fi / TCP-IP 栈就绪，然后起唯一的 80 端口 server，AP 与 STA 通吃。
    vTaskDelay(pdMS_TO_TICKS(3000));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /*
     * ★ 2026-11-12 关键修正：这个 web server 是「常驻」的（原来只有进配网 AP 模式才起），
     * 而 HTTPD_DEFAULT_CONFIG() 给的是 task_priority = 5、task_caps = INTERNAL：
     *   - 优先级 5 高于音频输出任务(4) —— 全系统唯一压在音频上面的任务；
     *   - 6KB 栈从内部 RAM 分配 —— 本板最紧缺的资源，直接挤掉 TLS/PCM 的连续块。
     * 「统一设置页」那版把这两件事一起带进来，正好是语音开始出问题的版本。
     * 三处收紧：
     *   1) 优先级压到 1（与 LVGL 同级）：永不抢占音频输出(4)与解码(2)；
     *   2) 栈挪到 PSRAM：把 6KB 内部 RAM 还给音频与 TLS；
     *   3) socket 7→3：每个连接还要额外吃约 1KB 内部缓冲，配置页用不到 7 个。
     */
    cfg.task_priority = tskIDLE_PRIORITY + 1;
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.stack_size = 6144;                 // 已挪 PSRAM，保持 6KB 无成本
    cfg.max_open_sockets = 3;
    cfg.max_uri_handlers = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // 支持 "/*" captive 兜底
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.lru_purge_enable = true;
    if (httpd_start(&g_httpd, &cfg) != ESP_OK) {
        ESP_LOGW(kTag, "httpd_start failed (port 80 busy?)");
        vTaskDelete(nullptr);
        return;
    }

    // 注册顺序有意义：esp_http_server 按注册顺序匹配，"/*" 必须放最后。
    httpd_uri_t root      = {"/",          HTTP_GET,  RootHandler,     nullptr};
    httpd_uri_t save      = {"/save",      HTTP_POST, SaveHandler,     nullptr};
    httpd_uri_t geo       = {"/geo",       HTTP_GET,  GeoHandler,      nullptr};
    httpd_uri_t showip    = {"/showip",    HTTP_GET,  ShowIpHandler,   nullptr};
    httpd_uri_t wifi_scan = {"/wifi/scan", HTTP_GET,  WifiScanHandler, nullptr};
    httpd_uri_t wifi_save = {"/wifi/save", HTTP_POST, WifiSaveHandler, nullptr};
    httpd_uri_t wifi_mode = {"/wifi/mode", HTTP_GET,  WifiModeHandler, nullptr};
    httpd_uri_t captive   = {"/*",         HTTP_GET,  CaptiveHandler,  nullptr};
    httpd_register_uri_handler(g_httpd, &root);
    httpd_register_uri_handler(g_httpd, &save);
    httpd_register_uri_handler(g_httpd, &geo);
    httpd_register_uri_handler(g_httpd, &showip);
    httpd_register_uri_handler(g_httpd, &wifi_scan);
    httpd_register_uri_handler(g_httpd, &wifi_save);
    httpd_register_uri_handler(g_httpd, &wifi_mode);
    httpd_uri_t status = {"/status", HTTP_GET, StatusHandler, nullptr};
    httpd_register_uri_handler(g_httpd, &status);
    httpd_register_uri_handler(g_httpd, &captive);

    char ip[16] = {};
    if (GetStationIp(ip, sizeof(ip))) {
        ESP_LOGW(kTag, "unified settings page ready: http://%s/ (STA)", ip);
    } else {
        ESP_LOGW(kTag, "unified settings page ready: http://192.168.4.1/ (config AP)");
    }
    vTaskDelete(nullptr);
}

}  // namespace

void AlphapiWeatherStart() {
    LoadLocation();
    if (xTaskCreate(WeatherTask, "weather", 4096, nullptr, 2, &g_weather_task) != pdPASS) {
        ESP_LOGW(kTag, "weather task create failed");
    }
    if (xTaskCreate(WebTask, "unified_web", 3072, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "settings page task create failed");
    }
}

bool AlphapiWeatherValid() { return g_valid; }
const char* AlphapiWeatherText() { return g_text; }
int AlphapiWeatherCode() { return g_code; }
int AlphapiWeatherTempC() { return g_temp; }

const char* AlphapiWeatherCity() {
    static char buf[32];
    const Location loc = Snapshot();
    std::memcpy(buf, loc.city, sizeof(buf));
    return buf;
}

bool AlphapiWeatherIp(char* buf, size_t len) { return GetStationIp(buf, len); }

bool AlphapiWeatherConfigured() { return g_configured; }

bool AlphapiWeatherIpVisible() {
    // 未配置时一直显示，引导用户打开设置页；配置完立刻隐藏。
    // /showip 可在 60 秒内临时叫回来，方便以后重新找到配置页。
    if (!g_configured) {
        return true;
    }
    return esp_timer_get_time() < g_ip_reveal_until_us;
}

void AlphapiWeatherRevealIp(int seconds) {
    if (seconds < 0) seconds = 0;
    g_ip_reveal_until_us = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000 * 1000;
}

bool AlphapiWeatherForecast(int idx, int* wmo_code, int* tmax_c) {
    if (idx < 0 || idx >= kForecastDays || !g_fc_valid[idx]) {
        return false;
    }
    if (wmo_code != nullptr) {
        *wmo_code = g_fc_code[idx];
    }
    if (tmax_c != nullptr) {
        *tmax_c = g_fc_tmax[idx];
    }
    return true;
}
