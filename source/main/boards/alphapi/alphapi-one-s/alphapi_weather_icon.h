#pragma once

#include "lvgl.h"

/*
 * 天气图标（AlphaPi One S 时钟页）—— 矢量图元绘制
 *
 * 为什么不用图标字体：本固件内置的 noto_sans_basic 字库里连汉字都没有，
 * 更不会有天气符号；而 LVGL 的 symbol 字体只有箭头/齿轮那类通用图标。
 *
 * 为什么用 LVGL 图元而不是画布位图：画布是 RGB565 无透明通道，必须自己填底色，
 * 屏上就多出一块深蓝色方块。这里改成「若干圆形 / 圆角矩形对象」拼图标 ——
 * 背景天然透明，边缘由 LVGL 矢量渲染，也不占位图缓冲。
 *
 * 一套几何两种尺寸：所有坐标按 16 单位网格定义，实际像素 = 单位 × size/16。
 * B 版首页里当前天气用 size=32（右栏，1 单位 = 2px 整像素），
 * 底部三天预报用 size=12（0.75px/单位），同一份图案等比收缩，不需要两套手绘。
 *
 * 屏幕位置：
 *   - 当前天气：32×32，内容区 (115, 4) = 屏幕 (115, 26)
 *   - 三天预报：12×12，屏 y104（内容区 y82）；x 由调用方按温度宽度动态给出（初始 8/58/108）
 */
namespace alphapi_weather_icon {

// 当前天气图标：16×16。
lv_obj_t* Render(lv_obj_t* parent, int wmo_code);

// 三天预报迷你图标：12×12，槽位 slot 取 0/1/2（今/明/后）。
lv_obj_t* RenderMini(lv_obj_t* parent, int slot, int wmo_code);

}  // namespace alphapi_weather_icon
