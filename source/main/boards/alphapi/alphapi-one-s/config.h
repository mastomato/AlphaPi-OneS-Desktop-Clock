#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * AlphaPi One S (ESP32-S2R2 / 8MB Flash / 2MB PSRAM / 160x128 TFT / 4 keys)
 *
 * 引脚来源：`小智+网络收音机\release\AlphaPi-One-S-完整技术手册.md` §5.2（实测）
 *           与 `AlphaPi_One_S_开发者手册_完整版.md` §2.1/2.2/2.3（两处一致）
 * 显示屏控制器：ST7735 类（代码里可用 st7789 驱动建句柄，但必须发 ST7735 初始化序列，
 *             MADCTL 最终为 0x60 —— 见手册 §5.1）
 */

/* ---------- Display (SPI2) ---------- */
#define DISPLAY_MOSI_GPIO   GPIO_NUM_38
#define DISPLAY_SCLK_GPIO   GPIO_NUM_37
#define DISPLAY_CS_GPIO     GPIO_NUM_34
#define DISPLAY_DC_GPIO     GPIO_NUM_36
#define DISPLAY_RESET_GPIO  GPIO_NUM_35
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_33
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_WIDTH   160
#define DISPLAY_HEIGHT  128
/* 手册 §5.1：swap_xy=true / mirror_x=true / mirror_y=false -> MADCTL=0x60 */
#define DISPLAY_SWAP_XY  true
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_SPI_MODE 0
#define DISPLAY_PIXEL_CLOCK_HZ (26 * 1000 * 1000)

/* ---------- Buttons (all active-low, internal pull-up) ---------- */
#define BUTTON_A_GPIO       GPIO_NUM_13
#define BUTTON_B_GPIO       GPIO_NUM_12
#define BUTTON_C_GPIO       GPIO_NUM_11
#define BUTTON_D_GPIO       GPIO_NUM_10
#define BUTTON_LONG_PRESS_MS 1000

/* ---------- Audio: N32 协处理器（UART 控制 + PCM 通过 UART 传输） ---------- */
/* 手册 §6.1：UART1，460800 8N1，无流控，TX=GPIO3，RX=GPIO0 */
#define AUDIO_UART_PORT      1
#define AUDIO_UART_TX_GPIO   GPIO_NUM_3
#define AUDIO_UART_RX_GPIO   GPIO_NUM_0
#define AUDIO_UART_BAUD      460800

/* ★ 输入与输出采样率【不同】，这是 2026-11-09 实测确定的，别乱统一！
 *
 * 录音（N32 采 → ESP 读 0x11）：22050 Hz
 *   依据：声明 24000 时，服务器后台抓到的录音「有点点变慢」（慢 8.8%）⇒ 实际约 22050。
 * 播放（ESP 写 0x15 → N32 放）：24000 Hz
 *   依据：声明 22050 时用户听到「声音快、而且尖锐些」（音调高 8.8%），
 *         且「说到一半就停」—— 因为按 22050 喂料比 N32 按 24000 消费慢，
 *         句子过半时 N32 前置缓冲见底就停。改成 24000 后两个症状同时消失。
 *   ⚠️ 2026-10-08 与 2026-11-09 上午曾两次把输出"改回 22050"，都导致语速/音调错误，
 *      不要再"按手册统一"。
 */
#define AUDIO_INPUT_SAMPLE_RATE   22050
#define AUDIO_OUTPUT_SAMPLE_RATE  24000
#define AUDIO_INPUT_CHANNELS      1
#define AUDIO_OUTPUT_CHANNELS     1

/* 硬件音量命令 0x14 的范围（手册 §6.3） */
#define AUDIO_HW_VOLUME_MIN  0
#define AUDIO_HW_VOLUME_MAX  6

/* 播放时 PCM 分块上限（手册 §6.4：≤200 字节） */
#define AUDIO_PCM_CHUNK_MAX  200   /* 先回到手册值；真实上限由固件里的载荷扫描测定 */

/* 本板音频是半双工（播放期间不能读麦克风），因此不是 duplex，也没有回声参考 */
#define AUDIO_DUPLEX          false
#define AUDIO_INPUT_REFERENCE false

/* ---------- LED / 电池 / 温度 ---------- */
/* 板上有电池与拨动电源开关，但“电池电量采集”引脚在现有文档里没有记载。
 * 先按“不支持”处理；若之后测出分压引脚，在此补 *_GPIO 并打开 BOARD_HAS_BATTERY。 */
// #define BOARD_HAS_BATTERY 1
// #define BATTERY_ADC_GPIO  GPIO_NUM_XX

#endif // _BOARD_CONFIG_H_
