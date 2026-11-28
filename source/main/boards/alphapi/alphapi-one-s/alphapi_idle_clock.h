#ifndef ALPHAPI_IDLE_CLOCK_H
#define ALPHAPI_IDLE_CLOCK_H

/*
 * 待机时钟页（AlphaPi One S）
 * ---------------------------
 * 设备处于「空闲 / 待机」状态时，用一块全屏卡片（大字时间 + 日期）替代表情；
 * 进入聆听、说话、联网、升级等状态时自动隐藏，界面回到原来的样子。
 *
 * 时间来源：SNTP（ntp.aliyun.com），首次联网后约 1~3 秒完成校时；
 * 校时完成前显示 "--:--"。
 *
 * 用法：在板子构造函数末尾调用 AlphapiIdleClockStart()。
 * 内部会延迟 3 秒（等 LVGL 和 SetupUI 就绪）再创建界面，不需要额外参数。
 */
void AlphapiIdleClockStart(void);

/* 确保 SNTP 已启动，并立刻重发一次查询（不等 60 秒轮询）。
 * 用于"等待校时"的循环里：Wi-Fi 刚连上时首次查询很可能已经错过。 */
void AlphapiSntpKick(void);

/*
 * 在 app_main 最早期调用（必须早于任何 HTTPS / WebSocket 连接）。
 *
 * 本板 SNTP 常年不成功，系统时间停在 1970/1881；mbedTLS 校验证书有效期因此
 * 必然失败（BADCERT_FUTURE / 0x801A），表现为 TLS handshake failed 且永不恢复。
 * 本函数用编译时刻给系统设一个落在证书有效期内的基准时间，不依赖网络、不占堆。
 * 若时间已经合理（SNTP 或上次校时留下的 RTC），则什么也不做。
 */
void AlphapiEnsureValidTime(void);

#endif  // ALPHAPI_IDLE_CLOCK_H
