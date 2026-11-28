/*
 * AlphaPi One S：本板的 app_main（在官方 main.cc 基础上只加了两个"开发便利"开关）
 *
 * 为什么需要这两个开关：本板没有 EN/GPIO0 复位电路、DTR/RTS 完全无效，而**冷启动是否进入
 * ROM 下载模式是随机的**（与 N32 协处理器在上电瞬间拉 GPIO0 的时序竞争有关）。有了它们，
 * 刷写不再靠碰运气。两个开关都可以直接置 0 关掉。
 *
 * 另附 AlphapiMarkStage()：把"启动进度"写进 flash 尾部扇区 0x7FF000 的调试机制
 * （主机侧 `esptool read-flash 0x7FF000 0x10` 读回最后到达的阶段）。默认编译为**空操作**，
 * 需要排障时把 ALPHAPI_ENABLE_STAGE_MARK 置 1 即可。板级/应用代码里的调用点保留不动。
 */
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "soc/rtc_cntl_reg.h"

#include "application.h"
#include "boards/alphapi/alphapi-one-s/alphapi_idle_clock.h"

#include <cstring>

#define TAG "main"

/* ---------- 开关 1：开机按住 D 键 → 复位后进 ROM 下载模式 ----------
 * 2026-11-11 v5 起默认关闭（原来都是 1）。收音机里长按 D 切回小智时，
 * esp_restart() 之后手指往往还按在 D 上；本固件 app_main 一进来就读到 D=1，
 * 立刻 ForceDownloadMode() 把芯片拖进 ROM 下载模式 —— 黑屏、无响应、只能断电，
 * 表现就是「切回小智死机」。四键 helper 同理可能被按键余电平误触发。
 * 现在刷机统一用六段包脚本的 --before default-reset 自动进下载模式，
 * 不再需要靠按键。两个宏保留为 0，排障时可手动临时置 1。 */
#define ALPHAPI_BOOT_TO_DOWNLOAD_ON_BUTTON_D 0
/* ---------- 开关 2：运行中按住 A+B+C+D 三秒 → 进 ROM 下载模式 ---------- */
#define ALPHAPI_FLASH_HELPER_TASK 0
/* ---------- 调试：把启动进度写入 flash 尾部（默认关；排障时置 1） ---------- */
#define ALPHAPI_ENABLE_STAGE_MARK 0
#define ALPHAPI_MARK_ADDR 0x7FF000

/* 下面两个 static 函数在开关关闭时不使用，显式标注避免 -Wunused-function */
#define ALPHAPI_MAYBE_UNUSED __attribute__((unused))

/* 本板实测：按键为**按下为高**（开发者手册写的"上拉、按下为低"是错的） */
static void ALPHAPI_MAYBE_UNUSED ConfigKeyInput(gpio_num_t pin) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
}

extern "C" void AlphapiMarkStage(uint8_t stage) {
#if ALPHAPI_ENABLE_STAGE_MARK
    static uint32_t seq = 0;
    uint8_t buf[16] = {0};
    std::memcpy(buf, "APMS", 4);
    buf[4] = stage;
    uint32_t s = ++seq;
    std::memcpy(&buf[6], &s, 4);
    esp_flash_erase_region(NULL, ALPHAPI_MARK_ADDR, 0x1000);
    esp_flash_write(NULL, buf, ALPHAPI_MARK_ADDR, sizeof(buf));
#else
    (void)stage;
#endif
}

static void ALPHAPI_MAYBE_UNUSED ForceDownloadMode(const char* why) {
    ESP_LOGW(TAG, "force ROM download mode: %s", why);
    AlphapiMarkStage(255);
    REG_WRITE(RTC_CNTL_OPTION1_REG,
              READ_PERI_REG(RTC_CNTL_OPTION1_REG) | RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

#if ALPHAPI_FLASH_HELPER_TASK
static void AlphapiFlashHelperTask(void*) {
    const gpio_num_t keys[] = {GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_10};  /* A B C D */
    for (auto k : keys) {
        ConfigKeyInput(k);
    }
    int held = 0;
    while (true) {
        bool all_pressed = true;
        for (auto k : keys) {
            if (gpio_get_level(k) == 0) {
                all_pressed = false;
                break;
            }
        }
        held = all_pressed ? held + 1 : 0;
        if (held >= 3) {          /* 3 × 1000ms */
            ForceDownloadMode("A+B+C+D held 3s");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

extern "C" void app_main(void)
{
    AlphapiMarkStage(1);          /* 1 = app_main 进入 */

#if ALPHAPI_FLASH_HELPER_TASK
    xTaskCreate(AlphapiFlashHelperTask, "alphapi_flash", 4096, nullptr, 1, nullptr);
#endif

#if ALPHAPI_BOOT_TO_DOWNLOAD_ON_BUTTON_D
    ConfigKeyInput(GPIO_NUM_10);              /* D 键 */
    if (gpio_get_level(GPIO_NUM_10) == 1) {   /* 按下为高 */
        ForceDownloadMode("button D held at boot");
    }
#endif

    /*
     * 双固件切槽的启动确认：radio.bin 用 esp_ota_set_boot_partition(ota_0) 切回来时，
     * 会把 otadata 写成 ESP_OTA_IMG_NEW / PENDING_VERIFY。若本固件不确认，
     * bootloader 的 rollback 重试计数耗尽后会滚回收音机，表现为「切不回来／反复黑屏」。
     * 开机立刻标记当前镜像有效；otadata 为空白时返回错误也无副作用。
     */
    esp_ota_mark_app_valid_cancel_rollback();

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    AlphapiMarkStage(2);          /* 2 = NVS 完成 */

    /*
     * 必须早于 Application::Initialize()：Wi-Fi 一连上就会立刻发 HTTPS 检查版本，
     * 那时系统时间若还停在 1970/1881，mbedTLS 会因证书「尚未生效」而确定性失败，
     * 重试多少次都不会好（TLS handshake failed 一直挂在屏幕上）。
     * 这里先用编译时刻给系统一个有效时间，SNTP / HTTP Date 之后再校准到真实值。
     */
    AlphapiEnsureValidTime();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    AlphapiMarkStage(3);          /* 3 = Application 单例完成 */

    app.Initialize();
    AlphapiMarkStage(24);         /* 24 = Initialize() 完成 */

    app.Run();  // This function runs the main event loop and never returns
    AlphapiMarkStage(25);
}
