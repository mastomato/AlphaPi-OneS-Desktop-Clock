/* AlphaPi One S 裸 SPI 显示后端（不使用 esp_lcd_panel_io_spi） */
#pragma once
#include <cstdint>
#include <cstddef>
#include "lvgl.h"

bool          RawSpiPanelInit(void);
void          RawSpiPanelFill(uint16_t color);
void          RawSpiPanelDraw(int x, int y, int w, int h, const uint16_t* pixels);
lv_display_t* RawSpiPanelRegisterDisplay(int width, int height);
