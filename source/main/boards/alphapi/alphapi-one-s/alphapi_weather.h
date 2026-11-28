#pragma once

/*
 * 天气数据 + 浏览器设置页（AlphaPi One S 待机时钟页用）
 *
 * 数据源：open-meteo.com，免密钥、支持明文 HTTP。明文响应的 Date 头还顺便把系统
 * 时间校准到真实值（本板 SNTP 常年不成功，时间不准曾导致 TLS 证书校验失败）。
 *
 * 位置不再写死：开机从 NVS 命名空间 "weather" 读取，缺省福州。
 *
 * 「统一设置页」：设备在 80 端口起唯一的 httpd，STA 与 AP 两种模式都能访问。
 * 页面最上面是 Wi-Fi 配网（状态 / 配网地址 / 扫描 / SSID 密码 / 进出配网模式），
 * 下面是天气位置设置。正常联网时访问屏幕右上角显示的设备 IP，配网模式下访问
 * http://192.168.4.1/，看到的是同一个页面 —— 不再有第二套配置页。
 * 保存位置后立刻重新拉天气，重启后仍然生效。
 *
 * 用法：板卡构造函数里调用 AlphapiWeatherStart()，时钟页每秒读下面的 getter。
 */

#include <cstddef>

// 启动后台任务：天气拉取 + 启动统一设置页 HTTP 服务（不再等 STA 拿到 IP）。
void AlphapiWeatherStart();

// 缺省位置（首次开机或 NVS 被清空时使用）
#ifndef ALPHAPI_WEATHER_LAT
#define ALPHAPI_WEATHER_LAT 26.0745f   /* 福州 */
#endif
#ifndef ALPHAPI_WEATHER_LON
#define ALPHAPI_WEATHER_LON 119.2965f
#endif

// 是否已经拿到过有效数据
bool AlphapiWeatherValid();

// 形如 "TEMP 24C SUN" / "TEMP --C"（还没拿到数据时的占位）
const char* AlphapiWeatherText();

// 原始 WMO 天气码；还没拿到数据时返回 -1（图标模块据此画占位图）
int AlphapiWeatherCode();

// 温度整数（摄氏）；还没数据时返回 INT_MIN
#include <climits>
int AlphapiWeatherTempC();

// 当前位置名称（UTF-8，供屏幕右下角显示）
const char* AlphapiWeatherCity();

// 当前 STA IP，形如 "192.168.1.23"；还没联网时返回 false
bool AlphapiWeatherIp(char* buf, size_t len);

// 是否已经在配置页里保存过位置（NVS 里有 lat/lon）。
// 没保存过时屏幕显示 IP 引导用户去设置；保存后 IP 自动消失，保持时钟页干净。
bool AlphapiWeatherConfigured();

// 屏幕上此刻是否应该显示 IP：未配置时一直显示，配置后仅在临时「重新显示」窗口内显示。
bool AlphapiWeatherIpVisible();

// 配置页 /showip 用：已配置也能临时把 IP 显示若干秒，方便以后再改设置。
void AlphapiWeatherRevealIp(int seconds);

// 未来三天预报（一次请求里顺带返回，不增加请求次数）。
// idx 取 0（今天）/ 1（明天）/ 2（后天）；拿到数据返回 true 并填 code / tmax_c。
bool AlphapiWeatherForecast(int idx, int* wmo_code, int* tmax_c);
