/*
 * AlphaPi One S — P1 灯带引脚自检（LED PIN PROBE）
 *
 * 目的：P1 扩展口的 5 个候选 GPIO（4/5/6/7/9）中，找出 RGB 灯带 DIN 接在哪个脚。
 *
 * 逻辑（纯 ESP-IDF，不依赖小智框架，避免被麦克风/音频链路干扰）：
 *   for gpio in {4,5,6,7,9}:
 *       在 gpio 上建 led_strip(RMT, WS2812, 8 灯)
 *       亮 5 秒 —— 颜色 = 该脚的"编码色"，用户看颜色就知道是哪个脚
 *       灭 1 秒（黑屏间隔，用来区分相邻的两脚）
 *       删掉 led_strip，释放 RMT 通道
 *   全部走完后停 3 秒，然后循环重新开始（方便用户反复观察）
 *
 * 颜色编码（用户只需看到"什么颜色"）：
 *   GPIO 4 → 红
 *   GPIO 5 → 绿
 *   GPIO 6 → 蓝
 *   GPIO 7 → 白
 *   GPIO 9 → 黄
 *
 * 开关：ALPHAPI_LED_PROBE，默认 0。
 * 打开方式：在 alphapi_one_audio_codec.cc 的 Start() 里会调用 AlphapiLedProbeRun()，
 *          或者由 main 里显式调用。见 alphapi_led_probe.h。
 */

#include "alphapi_led_probe.h"

#if ALPHAPI_LED_PROBE

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>
#include <driver/gpio.h>

#define TAG "LedProbe"

namespace {

struct ProbeStep {
    gpio_num_t gpio;
    uint8_t    r, g, b;
    const char* name;
};

/* 候选引脚 + 编码色（顺序即点亮顺序） */
const ProbeStep kSteps[] = {
    { GPIO_NUM_4, 255,   0,   0, "RED"    },   /* 红 */
    { GPIO_NUM_5,   0, 255,   0, "GREEN"  },   /* 绿 */
    { GPIO_NUM_6,   0,   0, 255, "BLUE"   },   /* 蓝 */
    { GPIO_NUM_7, 255, 255, 255, "WHITE"  },   /* 白 */
    { GPIO_NUM_9, 255, 255,   0, "YELLOW" },   /* 黄 */
};
constexpr int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

constexpr int kOnMs       = 5000;   /* 每个脚亮 5 秒 */
constexpr int kGapMs      = 1000;   /* 两脚之间灭 1 秒 */
constexpr int kRoundGapMs = 3000;   /* 一轮结束停 3 秒 */
constexpr uint16_t kLeds  = 8;      /* 一行灯珠数（按 8 处理；实际多少都能亮前 N 个） */

/* 在给定 GPIO 上建 led_strip，点亮一段时间，然后删除。
 * 每次新建/删除是为了避免 RMT 通道被前一个引脚占住。 */
bool RunOneStep(const ProbeStep& step)
{
    led_strip_handle_t strip = nullptr;

    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = step.gpio;
    cfg.max_leds       = kLeds;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    cfg.led_model      = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;   /* 10 MHz，WS2812 需要 ~0.3us 精度 */

    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &strip);
    if (err != ESP_OK || strip == nullptr) {
        ESP_LOGW(TAG, "GPIO %d : led_strip_new_rmt_device failed (%s)",
                 (int)step.gpio, esp_err_to_name(err));
        return false;
    }

    /* 全灯珠设为该脚编码色 */
    for (uint16_t i = 0; i < kLeds; i++) {
        led_strip_set_pixel(strip, i, step.r, step.g, step.b);
    }
    led_strip_refresh(strip);
    ESP_LOGI(TAG, "GPIO %d = %s  (ON %d ms)", (int)step.gpio, step.name, kOnMs);

    vTaskDelay(pdMS_TO_TICKS(kOnMs));

    /* 熄灭 */
    led_strip_clear(strip);
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(kGapMs));

    led_strip_del(strip);
    return true;
}

}  // namespace

void AlphapiLedProbeRun(void)
{
    static bool announced = false;

    ESP_LOGI(TAG, "===== P1 LED pin probe start (5 candidate GPIOs) =====");
    ESP_LOGI(TAG, "order: GPIO4=RED  GPIO5=GREEN  GPIO6=BLUE  GPIO7=WHITE  GPIO9=YELLOW");

    int round = 0;
    while (true) {
        round++;
        ESP_LOGI(TAG, "---- round %d ----", round);
        for (int i = 0; i < kStepCount; i++) {
            RunOneStep(kSteps[i]);
        }
        if (!announced) {
            announced = true;
            ESP_LOGI(TAG, "===== probe round 1 done, repeating =====");
        }
        vTaskDelay(pdMS_TO_TICKS(kRoundGapMs));
    }
}

#endif  /* ALPHAPI_LED_PROBE */
