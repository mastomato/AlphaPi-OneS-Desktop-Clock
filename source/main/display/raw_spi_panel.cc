/* AlphaPi One S 裸 SPI 显示后端
 *
 * 为什么不用 IDF 的 LCD 驱动（实测结论 2026-10-04）：
 *   只要创建 esp_lcd_panel_io_spi / esp_lcd_panel_st7789，面板就完全没有反应
 *   （连同时创建的裸 SPI 设备也会一并失效）；而纯 spi_master 发送同一份
 *   初始化序列 + 像素数据，面板可以正常显示。因此本板显示全部走 spi_master。
 *
 * 序列来源：板上厂商 MicroPython 驱动（ST7735.py）实际发出的字节流（抓取自板子）。
 */
#include "raw_spi_panel.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <esp_partition.h>

extern "C" void AlphapiSavePixelSnap(const uint16_t* px, size_t n);

/* 引脚与参数（与 boards/alphapi/alphapi-one-s/config.h 保持一致；
 * 这里内联是为了让本文件不依赖板级头文件的搜索路径） */
#ifndef ALPHAPI_PANEL_DEFS
#define ALPHAPI_PANEL_DEFS
#define PANEL_MOSI_GPIO  GPIO_NUM_38
#define PANEL_SCLK_GPIO  GPIO_NUM_37
#define PANEL_CS_GPIO    GPIO_NUM_34
#define PANEL_DC_GPIO    GPIO_NUM_36
#define PANEL_RESET_GPIO GPIO_NUM_35
#define PANEL_BL_GPIO    GPIO_NUM_33
#define PANEL_BL_INVERT  false
#define PANEL_WIDTH      160
#define PANEL_HEIGHT     128
#endif

#define TAG "RawSpiPanel"
#define ALPHAPI_FLUSH_CAPTURE 0  /* 取证已完成，关掉：它在刷新路径里擦写 flash，
                                  * 会短暂禁用 cache ⇒ 干扰 WiFi/TLS（开机 OTA 报 transmit failed）。 */

static spi_device_handle_t s_dev = nullptr;

static void RawCmd(uint8_t cmd, const uint8_t* data, size_t n)
{
    gpio_set_level(PANEL_DC_GPIO, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    spi_device_polling_transmit(s_dev, &t);
    if (n && data) {
        gpio_set_level(PANEL_DC_GPIO, 1);
        spi_transaction_t td = {};
        td.length = n * 8;
        td.tx_buffer = data;
        spi_device_polling_transmit(s_dev, &td);
    }
}

/* ★ 2026-10-18 官方固件逆向（决定性证据）：
 * 从官方 xiaozhi.bin 的 DROM(0x3f024cd0..0x3f024d10) 抠出完整 ST7735 初始化表，
 * gamma/PWCTR/FRMCTR 数值与本地 kSt7735InitCmds 完全一致，
 * 唯一关键差异：**官方 MADCTL = 0xC8（MY|MX|BGR）**。
 * 官方还发完整 gamma/PWCTR/FRMCTR 序列（不是出厂 MicroPython 的 st7789 短序列）。
 * 本函数照抄官方真值。 */
static void RawSeq(void)
{
    /* ★★ 2026-11-09 颜色问题最终定论（三轮扫描实验 + 用户逐轮确认）：
     *   症状：表情/文字整体偏水红（刷成紫色/偏色）。
     *   做法：让屏幕依次用 8 种组合显示"上红/中绿/下蓝"三条纯色，用户报出
     *         第 4 轮、第 8 轮颜色正确。
     *   两轮共同点 ⇒ **MADCTL 必须 0x60（BGR=0）** + **像素必须"高字节在前"**。
     *   反色位（INVOFF/INVON）对纯色无可辨影响，取 INVOFF。
     *   佐证：出厂 MicroPython ST7735 驱动 _pushcolor() 也是
     *         `colorData[0] = aColor >> 8`（高字节先）。
     *   ⇒ 之前一直发"低字节先" ⇒ 红蓝对调 ⇒ 整屏偏紫/水红。 */
    static const uint8_t mad2[] = {0x60};   /* MADCTL: MX|MV，BGR=0（RGB 顺序） */
    static const uint8_t col[]  = {0x05};   /* COLMOD RGB565 16bit（官方=0x05，非 st7789 的 0x55） */
    static const uint8_t frm[]  = {0x01, 0x2C, 0x2D};
    static const uint8_t frm3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    static const uint8_t inv[]  = {0x07};
    static const uint8_t pw1[]  = {0xA2, 0x02, 0x84};
    static const uint8_t pw2[]  = {0xC5};
    static const uint8_t pw3[]  = {0x0A, 0x00};
    static const uint8_t pw4[]  = {0x8A, 0x2A};
    static const uint8_t pw5[]  = {0x8A, 0xEE};
    static const uint8_t vm1[]  = {0x0E};
    static const uint8_t gp[]   = {0x0F,0x1A,0x0F,0x18,0x2F,0x28,0x20,0x22,
                                   0x1F,0x1B,0x23,0x37,0x00,0x07,0x02,0x10};
    static const uint8_t gn[]   = {0x0F,0x1B,0x0F,0x17,0x33,0x2C,0x29,0x2E,
                                   0x30,0x30,0x39,0x3F,0x00,0x07,0x03,0x10};
    static const uint8_t cs_def[] = {0x00,0x00,0x00,0x7F};
    static const uint8_t rs_def[] = {0x00,0x00,0x00,0x9F};

    RawCmd(0x01, nullptr, 0); vTaskDelay(pdMS_TO_TICKS(150));  /* SWRESET */
    RawCmd(0x11, nullptr, 0); vTaskDelay(pdMS_TO_TICKS(500));  /* SLPOUT */
    RawCmd(0xB1, frm, 3);    RawCmd(0xB2, frm, 3);    RawCmd(0xB3, frm3, 6);
    RawCmd(0xB4, inv, 1);
    RawCmd(0xC0, pw1, 3);    RawCmd(0xC1, pw2, 1);    RawCmd(0xC2, pw3, 2);
    RawCmd(0xC3, pw4, 2);    RawCmd(0xC4, pw5, 2);    RawCmd(0xC5, vm1, 1);
    RawCmd(0x20, nullptr, 0);    /* INVOFF —— INVON 实测整体反色，极性不是病因 */
    RawCmd(0x36, mad2, 1);   /* ★ MADCTL（见上） */
    RawCmd(0x3A, col, 1);    /* COLMOD 0x05 = 16bit RGB565 */
    RawCmd(0x2A, cs_def, 4); RawCmd(0x2B, rs_def, 4);  /* CASET/RASET 默认窗口（官方发 kColumnRange/kRowRange） */
    RawCmd(0xE0, gp, 16);    RawCmd(0xE1, gn, 16);
    RawCmd(0x29, nullptr, 0); vTaskDelay(pdMS_TO_TICKS(100));  /* DISPON */
    RawCmd(0x13, nullptr, 0); vTaskDelay(pdMS_TO_TICKS(10));   /* NORON */
}

/* 前置声明（色带实验已撤，无前向使用） */

bool RawSpiPanelInit(void)
{    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PANEL_BL_GPIO) | (1ULL << PANEL_DC_GPIO) |
                       (1ULL << PANEL_RESET_GPIO);
    cfg.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&cfg) != ESP_OK) return false;
    gpio_set_level(PANEL_BL_GPIO, PANEL_BL_INVERT ? 0 : 1);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = PANEL_MOSI_GPIO;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = PANEL_SCLK_GPIO;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = PANEL_WIDTH * PANEL_HEIGHT * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 40 * 1000 * 1000;    dev.mode = 0;
    dev.spics_io_num = PANEL_CS_GPIO;
    dev.queue_size = 4;
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return false;
    }

    /* 手动复位：拉低 100ms 再拉高（与实测一致） */
    gpio_set_level(PANEL_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PANEL_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    RawSeq();
    ESP_LOGW(TAG, "raw SPI panel ready %dx%d", PANEL_WIDTH, PANEL_HEIGHT);
    return true;
}

static void RawWindow(int x1, int y1, int x2, int y2, uint8_t* cs, uint8_t* rs)
{
    cs[0] = x1 >> 8; cs[1] = x1 & 0xFF; cs[2] = x2 >> 8; cs[3] = x2 & 0xFF;
    rs[0] = y1 >> 8; rs[1] = y1 & 0xFF; rs[2] = y2 >> 8; rs[3] = y2 & 0xFF;
    RawCmd(0x2A, cs, 4);
    RawCmd(0x2B, rs, 4);
    RawCmd(0x2C, nullptr, 0);
}

/* 静态传输缓冲：避免每次刷屏 malloc/free 造成堆碎片（TLS 需要连续内存） */
static uint8_t s_xfer_buf[1024];

/* 颜色映射已在 2026-11-09 用扫描实验定死（MADCTL 0x60 + 高字节在前），
 * 自检代码已删除。若将来换屏/换方向，参考 03-开发记录 里的"颜色 8 组合扫描"做法。 */

static void RawPush(const uint16_t* pixels, size_t count)
{
    /* ★ 颜色取证（第 5 轮）：把首个像素块的原始 u16 值 snapshot 到静态区，
     * 由板级诊断任务搬到 flash 0x5B0000（只写一次，不反复擦写）。
     * 目的：确认到达面板的像素到底是不是标准 RGB565 黄 0xFFE0。 */
    static bool s_snap_done = false;
    (void)s_snap_done;   /* 取证已收官（结论：面板极性反转），代码保留不启用 */
    uint8_t* buf = s_xfer_buf;
    gpio_set_level(PANEL_DC_GPIO, 1);
    size_t i = 0;
    while (i < count) {
        size_t n = count - i;
        if (n > 512) n = 512;
        for (size_t k = 0; k < n; k++) {
            /* ★★ 2026-11-09 结论：原始像素 **高字节在前** 直发，零变换。
             * （面板 MADCTL=0x60 BGR=0 + 高字节先 ⇒ 红显示红、蓝显示蓝。
             *   低字节先会让红蓝对调，表现为整屏偏紫/水红。）
             * 出厂 MicroPython 驱动 _pushcolor() 同样是 colorData[0]=aColor>>8。 */
            uint16_t p = pixels[i + k];
            buf[k * 2]     = static_cast<uint8_t>(p >> 8);
            buf[k * 2 + 1] = static_cast<uint8_t>(p & 0xFF);
        }
        spi_transaction_t t = {};
        t.length = n * 16;
        t.tx_buffer = buf;
        spi_device_polling_transmit(s_dev, &t);
        i += n;
    }
}

void RawSpiPanelDraw(int x, int y, int w, int h, const uint16_t* pixels)
{
    if (!s_dev || w <= 0 || h <= 0) return;
    uint8_t cs[4], rs[4];
    RawWindow(x, y, x + w - 1, y + h - 1, cs, rs);
    RawPush(pixels, (size_t)w * (size_t)h);
}

void RawSpiPanelFill(uint16_t color)
{
    if (!s_dev) return;
    uint8_t cs[4], rs[4];
    RawWindow(0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, cs, rs);
    uint16_t line[PANEL_WIDTH];
    for (int i = 0; i < PANEL_WIDTH; i++) line[i] = color;
    for (int y = 0; y < PANEL_HEIGHT; y++) RawPush(line, PANEL_WIDTH);
}

static void RawFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
#if ALPHAPI_FLUSH_CAPTURE
    /* ★取证（临时）：把第一次非纯色块的输入像素原样写 flash 0x5B0000。
     * 变换前的输入 = LVGL 实际输出的字节，判读它即可知道水红出自哪一层。 */
    {
        static bool s_captured = false;
        if (!s_captured) {
            size_t npx = (size_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
            const uint16_t* p = (const uint16_t*)px_map;
            bool has_color = false;
            size_t lim = npx; if (lim > 6400) lim = 6400;
            for (size_t i = 0; i < lim; i++) {
                uint16_t q = p[i];
                if (q == 0x0000 || q == 0xFFFF) continue;
                uint16_t r5 = (q >> 11) & 0x1F, b5 = q & 0x1F;
                if (r5 != b5) { has_color = true; break; }   /* 真·彩色（R!=B） */
            }
            if (has_color) {
                s_captured = true;
                const esp_partition_t* part = esp_partition_find_first(
                    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "ota_1");
                if (part) {
                    size_t nb = lim * 2; if (nb > 12288) nb = 12288;
                    static uint8_t s_cap[12288 + 32];
                    memcpy(s_cap, p, nb);
                    /* 头 16 字节标记：'FCAP' + w/h */
                    uint8_t hdr[16] = {'F','C','A','P',0,0,0,0,0,0,0,0,0,0,0,0};
                    hdr[4] = area->x2 - area->x1 + 1;
                    hdr[5] = area->y2 - area->y1 + 1;
                    esp_partition_erase_range(part, 0x2A0000, 0x5000);
                    esp_partition_write(part, 0x2A0000, hdr, 16);
                    esp_partition_write(part, 0x2A0010, s_cap, nb);
                }
            }
        }
    }
#endif
    if (!s_dev) return;
    RawSpiPanelDraw(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                    (const uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

lv_display_t* RawSpiPanelRegisterDisplay(int width, int height)
{
    lv_display_t* d = lv_display_create(width, height);
    if (d == nullptr) { ESP_LOGE(TAG, "lv_display_create failed"); return nullptr; }
    size_t buf_px = (size_t)width * 40;
    /* ★ 优先放 PSRAM：内部 RAM 要留给 mbedTLS（esp_tls_init 因堆不足返回 NULL 会导致 TLS 握手失败）
     * 我们的 flush 会经由下面那块小的静态缓冲转发，所以 LVGL 的绘制缓冲不必是 DMA 内存。 */
    void* buf = heap_caps_malloc(buf_px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) buf = heap_caps_malloc(buf_px * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == nullptr) buf = heap_caps_malloc(buf_px * 2, MALLOC_CAP_8BIT);
    if (buf == nullptr) { ESP_LOGE(TAG, "no draw buffer"); return nullptr; }
    lv_display_set_buffers(d, buf, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, RawFlushCb);
    ESP_LOGW(TAG, "lvgl display registered on raw SPI");
    return d;
}
