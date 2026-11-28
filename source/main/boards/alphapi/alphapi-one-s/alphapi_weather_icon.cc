#include "alphapi_weather_icon.h"

namespace alphapi_weather_icon {
namespace {

/*
 * 所有图案都按 16 单位网格定义，实际像素 = 单位 × size / 16。
 * 当前天气 size=16（1 单位 = 1px），三天预报 size=12（1 单位 = 0.75px，
 * 长度不足 1px 的描边按 1px 画，保证不消失）。
 *
 * 为什么不用画布：RGB565 画布没有透明通道，必须铺满底色，屏上就是一块深蓝方块。
 * 这里每个图元都是独立的 lv_obj（圆 / 圆角矩形），背景天然透明。
 */

constexpr int kUnits = 16;

constexpr uint32_t kSun = 0xFFD27A;    // 暖黄
constexpr uint32_t kCloud = 0xB8C4D6;  // 浅灰蓝
constexpr uint32_t kSnow = 0xE6EEF7;   // 近白
constexpr uint32_t kRain = 0x8FE3F0;   // 青
constexpr uint32_t kBolt = 0xFFD27A;   // 闪电暖黄

struct Painter {
    lv_obj_t* box;
    int size;

    int Len(float u) const {
        const int v = static_cast<int>(u * size / kUnits + 0.5f);
        return v < 1 ? 1 : v;
    }
    int Pos(float u) const { return static_cast<int>(u * size / kUnits + 0.5f); }

    lv_obj_t* Shape(int x, int y, int w, int h, uint32_t color, int radius) const {
        lv_obj_t* o = lv_obj_create(box);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, w, h);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, radius, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        return o;
    }

    // 圆头短条：半径取短边一半，视觉上就是预览稿里那些圆头小条
    void Bar(float x, float y, float w, float h, uint32_t color) const {
        const int pw = Len(w);
        const int ph = Len(h);
        Shape(Pos(x), Pos(y), pw, ph, color, (pw < ph ? pw : ph) / 2);
    }

    // 正圆
    void Disc(float cx, float cy, float d, uint32_t color) const {
        const int p = Len(d);
        Shape(Pos(cx - d / 2.0f), Pos(cy - d / 2.0f), p, p, color, LV_RADIUS_CIRCLE);
    }
};

/* ---------------- 单个图案（16 单位网格） ---------------- */

void Sun(const Painter& p) {
    p.Disc(8.0f, 8.0f, 6.6f, kSun);
    // 四向光须（圆头短条）
    p.Bar(7.15f, 0.8f, 1.7f, 2.2f, kSun);
    p.Bar(7.15f, 13.0f, 1.7f, 2.2f, kSun);
    p.Bar(0.8f, 7.15f, 2.2f, 1.7f, kSun);
    p.Bar(13.0f, 7.15f, 2.2f, 1.7f, kSun);
    // 四斜向光点
    p.Bar(2.4f, 2.4f, 1.6f, 1.6f, kSun);
    p.Bar(12.0f, 2.4f, 1.6f, 1.6f, kSun);
    p.Bar(2.4f, 12.0f, 1.6f, 1.6f, kSun);
    p.Bar(12.0f, 12.0f, 1.6f, 1.6f, kSun);
}

// 云：t 为云顶（单位），整体高 6.4，横向 2.2..13.8
void CloudAt(const Painter& p, float t, uint32_t color) {
    p.Disc(8.6f, t + 2.8f, 5.6f, color);         // 主鼓包
    p.Disc(5.4f, t + 4.0f, 4.8f, color);         // 左鼓包
    p.Disc(12.0f, t + 4.2f, 3.6f, color);        // 右鼓包
    p.Bar(2.2f, t + 4.6f, 11.6f, 1.8f, color);   // 底条压平下沿
}

void SunCloud(const Painter& p) {
    // 左上小太阳
    p.Disc(4.3f, 4.0f, 4.8f, kSun);
    p.Bar(3.7f, 0.6f, 1.3f, 1.3f, kSun);
    p.Bar(0.6f, 3.4f, 1.3f, 1.3f, kSun);
    CloudAt(p, 6.2f, kCloud);
}

void Overcast(const Painter& p) {
    CloudAt(p, 4.8f, kCloud);
}

void Rain(const Painter& p) {
    CloudAt(p, 2.0f, kCloud);
    p.Bar(4.0f, 9.4f, 1.2f, 3.4f, kRain);
    p.Bar(7.5f, 9.4f, 1.2f, 3.4f, kRain);
    p.Bar(11.0f, 9.4f, 1.2f, 3.4f, kRain);
}

void Storm(const Painter& p) {
    CloudAt(p, 2.0f, kCloud);
    // 之字闪电：三块小方，逐级向左下
    p.Bar(8.5f, 9.2f, 1.8f, 1.8f, kBolt);
    p.Bar(7.0f, 11.0f, 1.8f, 1.8f, kBolt);
    p.Bar(8.0f, 12.8f, 1.8f, 1.8f, kBolt);
}

void Snow(const Painter& p) {
    CloudAt(p, 2.0f, kCloud);
    p.Bar(3.8f, 10.6f, 1.6f, 1.6f, kSnow);
    p.Bar(7.2f, 11.8f, 1.6f, 1.6f, kSnow);
    p.Bar(10.6f, 10.6f, 1.6f, 1.6f, kSnow);
}

void Fog(const Painter& p) {
    p.Bar(2.0f, 3.6f, 12.0f, 1.7f, kCloud);
    p.Bar(4.2f, 6.5f, 7.6f, 1.7f, kCloud);
    p.Bar(2.0f, 9.4f, 12.0f, 1.7f, kCloud);
    p.Bar(4.2f, 12.3f, 7.6f, 1.7f, kCloud);
}

void Paint(lv_obj_t* box, int size, int code) {
    const Painter p = {box, size};
    if (code < 0) {
        Overcast(p);   // 还没数据：一朵灰云占位
        return;
    }
    if (code == 0) {
        Sun(p);
    } else if (code <= 2) {
        SunCloud(p);
    } else if (code == 3) {
        Overcast(p);
    } else if (code <= 48) {
        Fog(p);
    } else if (code <= 67) {
        Rain(p);
    } else if (code <= 77) {
        Snow(p);
    } else if (code <= 82) {
        Rain(p);
    } else if (code <= 86) {
        Snow(p);
    } else {
        Storm(p);
    }
}

lv_obj_t* MakeBox(lv_obj_t* parent, int size, int x, int y) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);        // 无背景、无边框、无内边距 → 完全透明
    lv_obj_set_size(box, size, size);
    lv_obj_set_pos(box, x, y);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

/* ---------------- 槽位缓存 ---------------- */

constexpr int kMiniSlots = 3;
constexpr int kMiniPitch = 50;   // B 版底部预报行：段起点 x 8 / 58 / 108

lv_obj_t* g_big = nullptr;
int g_big_code = -2;
lv_obj_t* g_mini[kMiniSlots] = {};
int g_mini_code[kMiniSlots] = {-2, -2, -2};

}  // namespace

lv_obj_t* Render(lv_obj_t* parent, int wmo_code) {
    if (parent == nullptr) {
        return nullptr;
    }
    if (g_big == nullptr) {
        /* B 版首页：当前天气图标 32×32 放右栏，内容区 (115,4) = 屏幕 (115,26)。
         * size=32 时 1 单位 = 2px，几何全部落在整像素上。 */
        g_big = MakeBox(parent, 32, 115, 4);
    }
    if (wmo_code != g_big_code) {
        lv_obj_clean(g_big);   // 换天气类型时重建图元（一天只发生几次）
        Paint(g_big, 32, wmo_code);
        g_big_code = wmo_code;
    }
    return g_big;
}

lv_obj_t* RenderMini(lv_obj_t* parent, int slot, int wmo_code) {
    if (parent == nullptr || slot < 0 || slot >= kMiniSlots) {
        return nullptr;
    }
    if (g_mini[slot] == nullptr) {
        /* B 版底部预报行（屏幕 y104 → 内容区 y82）：这里只给一个初始 x，
         * 实际位置由 alphapi_idle_clock.cc 的 UpdateText 按「温度文本实际宽度 + 1」
         * 用 lv_obj_set_pos 覆盖 —— 因为顺序是「日名 · 温度 · 图标」，
         * 温度宽窄不一（"--" / "26°" / "-10"），图标必须跟着温度右缘走。 */
        g_mini[slot] = MakeBox(parent, 12, 8 + slot * kMiniPitch, 82);
    }
    if (wmo_code != g_mini_code[slot]) {
        lv_obj_clean(g_mini[slot]);
        Paint(g_mini[slot], 12, wmo_code);
        g_mini_code[slot] = wmo_code;
    }
    return g_mini[slot];
}

}  // namespace alphapi_weather_icon
