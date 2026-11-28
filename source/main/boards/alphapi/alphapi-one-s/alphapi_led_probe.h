#ifndef _ALPHAPI_LED_PROBE_H_
#define _ALPHAPI_LED_PROBE_H_

/*
 * P1 灯带引脚自检。
 *
 * ALPHAPI_LED_PROBE = 1 时，调用 AlphapiLedProbeRun() 会在 P1 的 5 个候选 GPIO
 * （4/5/6/7/9）上依次点亮不同颜色，用来确定灯带 DIN 接在哪个脚：
 *
 *     GPIO 4 → 红    GPIO 5 → 绿    GPIO 6 → 蓝
 *     GPIO 7 → 白    GPIO 9 → 黄
 *
 * 每个脚亮 5 秒，中间灭 1 秒，一轮 32 秒，然后循环。
 * 该函数【不会返回】，正常情况下应在独立任务里跑。
 *
 * 默认关闭（0）。测出引脚后应关回 0，再改用正式的 led_strip_control。
 */

#ifndef ALPHAPI_LED_PROBE
#define ALPHAPI_LED_PROBE 0
#endif

#if ALPHAPI_LED_PROBE
extern "C" void AlphapiLedProbeRun(void);
#endif

#endif  // _ALPHAPI_LED_PROBE_H_
