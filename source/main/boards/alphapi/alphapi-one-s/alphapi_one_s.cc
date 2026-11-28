#include "wifi_board.h"
#include "alphapi_one_audio_codec.h"
#include "config.h"
#include "raw_spi_panel.h"
#include "alphapi_led_probe.h"
#include "alphapi_idle_clock.h"

/* 在 codec 文件里实现：启动屏幕诊断任务 */
extern "C" void AlphapiStartAudioDiag(void);

#include "application.h"
#include "button.h"
#include "display/lcd_display.h"
#include "assets/lang_config.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "alphapi_weather.h"
#include <wifi_manager.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <algorithm>
#include <vector>

#define TAG "AlphaPiOneS"

/*
 * 双固件切槽：D 长按 -> 把启动分区指向 ota_1（网络收音机）并重启。
 * radio.bin 那侧有反向逻辑（长按 D 切回 ota_0）。只改 otadata，不碰任何 app 镜像，
 * 所以 ota_1 里的收音机固件可以原样保留。
 */
static void SwitchToRadioFirmware() {
    /*
     * 切回保护：radio.bin 里是「长按 D 约 3 秒」才切回小智，esp_restart() 后
     * 手指往往还按在 D 上。本固件若刚启动就把这个残留按键识别成 D 长按，
     * 会立刻又切回收音机，形成来回弹跳／看起来像死机。esp_timer 从芯片启动
     * 开始计时，开机 5 秒内一律忽略切槽长按。
     */
    if (esp_timer_get_time() < 5LL * 1000 * 1000) {
        ESP_LOGW(TAG, "ignore D long press during boot grace period");
        return;
    }
    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "ota_1");
    if (target == nullptr) {
        /* 某些分区表不带 label，退而按 subtype 0x11 找 */
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)0x11, nullptr);
    }
    if (target == nullptr) {
        ESP_LOGE(TAG, "ota_1 not found, stay on xiaozhi");
        return;
    }
    ESP_LOGW(TAG, "switch to radio firmware @ 0x%08X", (unsigned)target->address);
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed, stay on xiaozhi");
        return;
    }
    /*
     * 收音机固件不会调用 esp_ota_mark_app_valid_cancel_rollback()。
     * 上面这步在 CONFIG_APP_ROLLBACK_ENABLE=y 时会把新槽写成 ESP_OTA_IMG_NEW，
     * bootloader 再改成 PENDING_VERIFY 并启动 rollback 计数；收音机不确认，
     * 切回时就容易被判成「上次镜像未验证」。这里把刚写入的最高 seq 条目
     * 直接标记为 VALID，两张固件都不再背 PENDING_VERIFY 包袱。
     */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "otadata ready, restarting into radio");
    vTaskDelay(pdMS_TO_TICKS(100));   // 让日志和 otadata 写操作收尾
    esp_restart();
}

/*
 * 启动进度标记（实现在 main.cc）：把阶段编号写入 flash 尾部扇区 0x7FF000。
 * 主机侧用 esptool read-flash 0x7FF000 0x10 即可读到"最后到达的阶段"，无需日志/屏幕。
 * 阶段编号约定见 main.cc 顶部注释。
 */
extern "C" void AlphapiMarkStage(uint8_t stage);

/* 置 1 则改用 st7789 驱动的内置初始化序列（默认 0：手发 ST7735 序列，与官方固件一致） */
#ifndef ALPHAPI_USE_BUILTIN_PANEL_INIT
#define ALPHAPI_USE_BUILTIN_PANEL_INIT 0
#endif

/*
 * 开机自检（排障用）：先直接用 GPIO 拉高背光（绕过 LEDC 背光驱动），再把整屏依次刷成
 * 红 → 绿 → 蓝 → 白。用途是定位"黑屏"卡在哪一层：
 *   四色依次闪过   → app 已在运行、面板与背光都通 → 问题在后面（LVGL/UI 或别的子系统）
 *   停在某一色不动 → app 在该步之后崩了（由此可知崩溃点）
 *   全黑           → 面板初始化/背光/引脚，或 app 根本没跑起来
 * 排障结束后把 ALPHAPI_BOOT_SELFTEST 改成 0 重编即可。
 */
#ifndef ALPHAPI_BOOT_SELFTEST
#define ALPHAPI_BOOT_SELFTEST 0   /* 开机不再闪背光/闪四色（排障结束） */
#endif


/*
 * AlphaPi One S 板级实现（上游 v2.4.2 缺失的那一块）
 *
 * 依据：`AlphaPi_One_S_开发者手册_完整版.md` §10 与 `AlphaPi-One-S-完整技术手册.md`
 *   - 显示屏：ST7735 类控制器 + SPI2（MOSI=38 / SCLK=37 / CS=34 / DC=36 / RST=35），背光 PWM=33
 *   - 音频  ：自定义 UART 协处理器（见 alphapi_one_audio_codec.*），24000Hz 单声道 8bit
 *   - 按键  ：A=13 / B=12 / C=11 / D=10（上拉，按下为低）
 *   - 小智模式按键：A=对话开关，B/C=音量，D=静音（与官方一致）
 *   - 额外补上官方包缺失的能力：**长按 D 进配网**（官方包实测 A+D 无效，见根 README §“已知差异”）
 */

struct AlphaPiPanelCmd {
    uint8_t cmd;
    const uint8_t* data;
    size_t len;
    uint32_t delay_ms;
};

static const uint8_t kColmodRgb565[] = {0x05};              /* RGB565 */
static const uint8_t kMadctlZero[] = {0x00};   /* 已不用（MADCTL 统一走 kMadctlRot0=0x08）*/
static const uint8_t kCaset[] = {0x00,0x00,0x00,0x7F};  /* 列 0..127 */
static const uint8_t kRaset[] = {0x00,0x00,0x00,0x9F};  /* 行 0..159 */
static const uint8_t kMadctlRot0[]   = {0x08};              /* bit3=RGB：修正红蓝颠倒（黄显示成紫），2026-10-16 用户实测 */

/* ST7735 完整初始化序列（手册 §4.2 + §10.4.2 列出的全部命令组，值取标准 ST7735R 序列）。
 * 历史：本移植最初只发 SLPOUT/COLMOD/DISPON 三条 —— 板子**不显示**（背光亮但无画面）；
 * 改成下面这套完整序列后，面板立刻正常工作（实测 2026-10-03：红→绿→蓝→白四屏依次可见）。 */
static const uint8_t kFrmctr1[] = {0x01, 0x2C, 0x2D};                     /* FRMCTR1 */
static const uint8_t kFrmctr2[] = {0x01, 0x2C, 0x2D};                     /* FRMCTR2 */
static const uint8_t kFrmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};   /* FRMCTR3 */
static const uint8_t kInvctr[]  = {0x07};                                 /* INVCTR  */
static const uint8_t kPwctr1[]  = {0xA2, 0x02, 0x84};                     /* PWCTR1  */
static const uint8_t kPwctr2[]  = {0xC5};                                 /* PWCTR2  */
static const uint8_t kPwctr3[]  = {0x0A, 0x00};                           /* PWCTR3  */
static const uint8_t kPwctr4[]  = {0x8A, 0x2A};                           /* PWCTR4  */
static const uint8_t kPwctr5[]  = {0x8A, 0xEE};                           /* PWCTR5  */
static const uint8_t kVmctr1[]  = {0x0E};                                 /* VMCTR1  */
static const uint8_t kGmctrp1[] = {0x0F,0x1A,0x0F,0x18,0x2F,0x28,0x20,0x22,
                                   0x1F,0x1B,0x23,0x37,0x00,0x07,0x02,0x10};  /* GMCTRP1(实测组) */
static const uint8_t kGmctrn1[] = {0x0F,0x1B,0x0F,0x17,0x33,0x2C,0x29,0x2E,
                                   0x30,0x30,0x39,0x3F,0x00,0x07,0x03,0x10};  /* GMCTRN1(实测组) */

static const AlphaPiPanelCmd kSt7735InitCmds[] = {
    {0x01, nullptr,     0, 150},      /* SWRESET 软复位 */
    {0x11, nullptr,     0, 500},      /* SLPOUT  退出睡眠 */
    {0xB1, kFrmctr1,    3, 0},        /* 帧率控制 1 */
    {0xB2, kFrmctr2,    3, 0},        /* 帧率控制 2 */
    {0xB3, kFrmctr3,    6, 0},        /* 帧率控制 3 */
    {0xB4, kInvctr,     1, 0},        /* 反转控制 */
    {0xC0, kPwctr1,     3, 0},        /* 电源控制 1 */
    {0xC1, kPwctr2,     1, 0},        /* 电源控制 2 */
    {0xC2, kPwctr3,     2, 0},        /* 电源控制 3 */
    {0xC3, kPwctr4,     2, 0},        /* 电源控制 4 */
    {0xC4, kPwctr5,     2, 0},        /* 电源控制 5 */
    {0xC5, kVmctr1,     1, 0},        /* VCOM 控制 */
    {0x20, nullptr,     0, 0},        /* INVOFF 反转关 */
    {0x36, kMadctlRot0, 1, 0},        /* MADCTL 方向 */
    {0x3A, kColmodRgb565, 1, 0},      /* COLMOD RGB565 */
    {0x2A, kCaset,      4, 0},        /* CASET 列窗口 0..127（实测序列显式发送） */
    {0x2B, kRaset,      4, 0},        /* RASET 行窗口 0..159 */
    {0xE0, kGmctrp1,   16, 0},        /* 正伽马校正 */
    {0xE1, kGmctrn1,   16, 0},        /* 负伽马校正 */
    {0x29, nullptr,     0, 100},      /* DISPON 显示开 */
    {0x13, nullptr,     0, 10},       /* NORON 正常显示模式 */
    {0x36, kMadctlRot0, 1, 0},       /* MADCTL 最终值：0x08 = RGB 序（修正红蓝颠倒） */
};

class AlphaPiOneSBoard : public WifiBoard {
public:
    AlphaPiOneSBoard() {
        AlphapiMarkStage(4);           /* 4 = 板子构造函数进入 */
#if ALPHAPI_BOOT_SELFTEST
        /*
         * 调试探针（不属于正常功能）：以 1 秒亮/1 秒灭 的慢速交替闪 10 秒。
         * 目的：不论背光是高有效还是低有效都能看见闪，从而证明
         *   "app 已经走到板子构造函数"（而不是卡在更早的初始化里）。
         */
        gpio_config_t dbg_bl = {};
        dbg_bl.pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN;
        dbg_bl.mode = GPIO_MODE_OUTPUT;
        gpio_config(&dbg_bl);
        for (int i = 0; i < 10; i++) {
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, i % 2);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);
#endif
        AlphapiMarkStage(5);           /* 5 = 慢闪探针结束 */
        InitializeDisplay();
        AlphapiMarkStage(15);          /* 15 = InitializeDisplay 返回 */
        InitializeButtons();
        AlphapiMarkStage(16);          /* 16 = 四个按键对象创建完成 */
        GetBacklight()->RestoreBrightness();
        AlphapiMarkStage(17);          /* 17 = 构造函数全部走完 */

        /*
         * 网络收音机不在本固件内：它是 ota_1 里独立的一整套固件，D 长按切槽重启。
         * 这样本固件不必链接 MP3 解码/电台页，内部 RAM 才有余量完成 TLS 握手。
         */
        AlphapiMarkStage(18);          /* 18 = 构造函数进入 UI 初始化阶段 */

        /*
         * 待机时钟页：空闲时用「时间 + 日期」全屏替代表情（见 alphapi_idle_clock.cc）。
         * 内部延迟 3 秒建 UI，等 LVGL 与 SetupUI 就绪；非空闲状态自动隐藏，不影响对话界面。
         */
        AlphapiIdleClockStart();
        /* 天气：联网后拉一次，之后每 15 分钟一次（明文 HTTP，避开 TLS） */
        AlphapiWeatherStart();

#if ALPHAPI_LED_PROBE
        /*
         * P1 灯带引脚自检（见 alphapi_led_probe.cc）。
         * 延迟 8 秒等系统启动完（屏幕/网络都起来），再开始循环点亮候选 GPIO，
         * 这样用户看到的灯色不会被启动过程干扰。
         * 独立任务，栈 3072，优先级 5（低，不影响音频）。
         */
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(8000));
            AlphapiLedProbeRun();      /* 不返回 */
            vTaskDelete(nullptr);
        }, "led_probe", 3072, nullptr, 5, nullptr);
#endif
    }

    virtual std::string GetBoardType() override { return "alphapi-one-s"; }

    virtual AudioCodec* GetAudioCodec() override {
        static AlphaPiOneAudioCodec audio_codec(
            static_cast<uart_port_t>(AUDIO_UART_PORT), AUDIO_UART_TX_GPIO, AUDIO_UART_RX_GPIO,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_UART_BAUD);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    /*
     * 调试路标：把整屏刷成指定 RGB565 颜色。
     * 用途：app 卡在启动流程某一步时，屏幕颜色就是"最后到达的路标"。
     * 应用侧通过 AlphapiMilestonePaint() 调用（见 application.cc 的临时插桩）。
     * LVGL 一旦开始绘制就会覆盖它 —— 所以"看到界面"本身就说明流程走完了。
     */
    static void PaintMilestoneColor(uint16_t color) {
        if (panel_ == nullptr) {
            return;
        }
        static std::vector<uint16_t> line;
        line.assign(static_cast<size_t>(DISPLAY_WIDTH), color);
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            esp_lcd_panel_draw_bitmap(panel_, 0, y, DISPLAY_WIDTH, y + 1, line.data());
        }
        ESP_LOGW(TAG, "MILESTONE 0x%04X", color);
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    /*
     * 电池：板上有电池与拨动电源开关，但现有文档没有记下电量采集引脚，
     * 因此暂不实现 GetBatteryLevel（基类默认返回 false，界面不显示电量）。
     * 测出分压引脚后：override 之，并在 config.h 里补 BATTERY_ADC_GPIO。
     */

private:
    Display* display_ = nullptr;
    static esp_lcd_panel_handle_t panel_;
    Button* button_a_ = nullptr;
    Button* button_b_ = nullptr;
    Button* button_c_ = nullptr;
    Button* button_d_ = nullptr;

#if ALPHAPI_BOOT_SELFTEST
    void RunBootSelfTest(esp_lcd_panel_handle_t panel) {
        /* 裸 SPI 四色自检：屏幕上依次刷 红/绿/蓝/白，每色 700ms */
        static const uint16_t kColors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        for (int c = 0; c < 4; c++) {
            RawSpiPanelFill(kColors[c]);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        ESP_LOGW(TAG, "SELFTEST: raw SPI four-color test done");
    }
#endif

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {

        /* ★ 2026-10-04 实测结论：本板不能用 IDF 的 LCD 驱动
         * （esp_lcd_panel_io_spi / esp_lcd_panel_st7789）。只要创建它们，面板就完全无反应；
         * 纯 spi_master 发同一份序列可正常显示 —— 故这里改用裸 SPI 后端，
         * 下面原有的 IDF LCD 代码全部跳过（保留以备参考）。 */
        AlphapiMarkStage(6);
        if (!RawSpiPanelInit()) {
            ESP_LOGE(TAG, "raw SPI panel init failed");
        }
        AlphapiMarkStage(12);
        panel_ = nullptr;
        /* 屏幕诊断任务（音频计数 + TLS 内层错误码）：串口读不到，只能看屏幕 */
        AlphapiStartAudioDiag();
        display_ = new SpiLcdDisplay(nullptr, nullptr, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        AlphapiMarkStage(13);
        return;

        AlphapiMarkStage(6);           /* 6 = 进入显示初始化 */
        InitializeSpi();
        AlphapiMarkStage(7);           /* 7 = SPI2 总线就绪 */

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        // 手册 §5.3：trans_queue_depth=0 让 draw_bitmap 变同步，是官方修掉 DMA 闪屏的关键
        io_config.trans_queue_depth = 0;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));
        AlphapiMarkStage(8);           /* 8 = panel IO 就绪 */

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_GPIO;
        panel_config.bits_per_pixel = 16;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        /* 2026-10-16 用户实测：黄色表情显示成紫色 = 红蓝通道颠倒 ⇒ 改 BGR */
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#else
        panel_config.color_space = ESP_LCD_COLOR_SPACE_BGR;
#endif

        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        AlphapiMarkStage(9);           /* 9 = panel 句柄就绪 */
        /* 手动复位：拉低 100ms 再拉高（与实测能点亮的 draw.py 一致） */
        {
            gpio_config_t rst_cfg = {};
            rst_cfg.pin_bit_mask = 1ULL << DISPLAY_RESET_GPIO;
            rst_cfg.mode = GPIO_MODE_OUTPUT;
            gpio_config(&rst_cfg);
            gpio_set_level(DISPLAY_RESET_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(DISPLAY_RESET_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        AlphapiMarkStage(10);          /* 10 = 硬件复位完成 */

        /*
         * ★ 关键修正（2026-10-04 实测得出）：
         * 先用驱动自己的 esp_lcd_panel_init()，再发厂商那套 ST7735 序列。
         * 依据：在板子上跑厂商 MicroPython 驱动抓到的字节流与我们的序列完全一致却点不亮，
         *       唯一结构性差别就是"是否先让驱动初始化过面板"。厂商 ESP-IDF 固件也是两套都发。
         */
        /* 实测结论：这里**不能**调 esp_lcd_panel_init()。
         * 它发的是 ST7789 的初始化，会先把面板置成脏状态，之后我们再发 ST7735 序列也救不回来。
         * （2026-10-04：裸 SPI 单独发同一份序列可以正常显示，证明序列本身没问题。） */
        AlphapiMarkStage(11);          /* 11a = 跳过驱动 init */

        /*
         * 关键：这里**不调用** esp_lcd_panel_init()。
         * 本板实际是 ST7735 类控制器（手册 §4.1），官方固件也是用 st7789 驱动句柄
         * 自己发 ST7735 初始化序列。若出现花屏/黑屏，把 ALPHAPI_USE_BUILTIN_PANEL_INIT 改成 1 对比。
         */
#if !ALPHAPI_USE_BUILTIN_PANEL_INIT
        for (const auto& c : kSt7735InitCmds) {
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io, c.cmd, c.data, c.len));
            if (c.delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
            }
        }
#else
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#endif
        AlphapiMarkStage(11);          /* 11 = ST7735 初始化序列发送完成 */

        /* 按实测能点亮的 MicroPython 路径：这三个调用会重写 MADCTL，必须去掉
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        */
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        AlphapiMarkStage(12);          /* 12 = 显示已打开 */

        panel_ = panel;      /* 供 PaintMilestoneColor() 使用（调试路标） */

#if ALPHAPI_BOOT_SELFTEST
        RunBootSelfTest(panel);
#endif
        AlphapiMarkStage(13);          /* 13 = 四色自检完成 */

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        AlphapiMarkStage(14);          /* 14 = LVGL 显示对象创建完成 */
    }

    void ToggleChatOrEnterWifiConfig() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            // 启动阶段按任意键 = 进配网（与上游 df-k10 等板一致）
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void AdjustVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void ToggleWifiConfigMode() {
        auto& app = Application::GetInstance();
        if (IsInWifiConfigMode() || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            /*
             * 已在配网模式：再长按一次 A = 退出配网、切回正常的时钟/小智界面。
             * WifiBoard::EnterWifiConfigMode() 在 kDeviceStateWifiConfiguring 状态下
             * 会直接 return（只放 Speaking/Notifying/Listening/Idle/Starting 通过），
             * 所以这里必须自己处理，否则进了配网就再也出不来。
             * StopConfigAp() 会发 ConfigModeExit 事件，WifiBoard 收到后自动
             * TryWifiConnect()：新配的 SSID 立即生效，没配完也能用旧凭据重连。
             */
            ESP_LOGI(TAG, "A long press: leaving wifi config mode");
            GetDisplay()->ShowNotification(std::string("Exiting WiFi config"));
            app.Schedule([]() {
                WifiManager::GetInstance().StopConfigAp();
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            });
            return;
        }
        EnterWifiConfigMode();
    }

    void InitializeButtons() {
        // 双固件模式（本固件 = 小智），按键与别人那版双固件 README 对齐：
        //   A 短按=对话（启动阶段=进配网）
        //   A 长按 3 秒=进/出配网（toggle，配完网再长按一次即可切回主界面）
        //   B/C 短按=音量减/加，D 短按=静音，D 长按=切到 ota_1 网络收音机并重启
        // 为什么配网要在 A：原来 D 长按是配网，但 D 长按已用于切槽，必须把配网挪走。
        // A 单独用 3 秒阈值（其余键仍是 config.h 的 1000ms），避免日常握持误触进 AP。
        constexpr uint16_t kConfigLongPressMs = 3000;
        button_a_ = new Button(BUTTON_A_GPIO, true, kConfigLongPressMs);
        button_a_->OnClick([this]() { ToggleChatOrEnterWifiConfig(); });
        button_a_->OnLongPress([this]() { ToggleWifiConfigMode(); });

        button_b_ = new Button(BUTTON_B_GPIO, true, BUTTON_LONG_PRESS_MS);
        button_b_->OnClick([this]() { AdjustVolume(-10); });

        button_c_ = new Button(BUTTON_C_GPIO, true, BUTTON_LONG_PRESS_MS);
        button_c_->OnClick([this]() { AdjustVolume(+10); });

        button_d_ = new Button(BUTTON_D_GPIO, true, BUTTON_LONG_PRESS_MS);
        button_d_->OnClick([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(codec->output_volume() > 0 ? 0 : 70);
        });
        button_d_->OnLongPress([]() { SwitchToRadioFirmware(); });
    }
};

DECLARE_BOARD(AlphaPiOneSBoard);

esp_lcd_panel_handle_t AlphaPiOneSBoard::panel_ = nullptr;

/* 供 application.cc 等上游文件调用的调试路标（其它板型不需要，链接不到也只会是空实现 */
extern "C" void AlphapiMilestonePaint(uint16_t color) {
    AlphaPiOneSBoard::PaintMilestoneColor(color);
}
