/*******************************************************************************
 * Size: 12 px
 * Bpp: 4
 * Opts: --font C:/Windows/Fonts/segoeuib.ttf --range 0xB0,0x2D,0x30-0x39 --size 12 --bpp 4 --format lvgl --no-compress --lv-font-name alphapi_num_12 -o C:/esp/xiaozhi-clock/main/boards/alphapi/alphapi-one-s/alphapi_num_12.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef ALPHAPI_NUM_12
#define ALPHAPI_NUM_12 1
#endif

#if ALPHAPI_NUM_12

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+002D "-" */
    0x0, 0x0, 0x2, 0xff, 0xf0, 0x3, 0x33, 0x0,

    /* U+0030 "0" */
    0x3, 0xdf, 0xc3, 0x0, 0xee, 0x7e, 0xf0, 0x6f,
    0x70, 0x8f, 0x58, 0xf5, 0x6, 0xf7, 0x8f, 0x40,
    0x6f, 0x66, 0xf7, 0x8, 0xf4, 0x1f, 0xe7, 0xfe,
    0x0, 0x4d, 0xfc, 0x30,

    /* U+0031 "1" */
    0x4, 0xde, 0xbf, 0xfe, 0x64, 0xfe, 0x0, 0xfe,
    0x0, 0xfe, 0x0, 0xfe, 0x0, 0xfe, 0x0, 0xfe,

    /* U+0032 "2" */
    0x9, 0xef, 0xc3, 0x1, 0xd8, 0x8f, 0xe0, 0x0,
    0x0, 0xdf, 0x0, 0x0, 0x5f, 0xb0, 0x0, 0x9f,
    0xb0, 0x0, 0xbf, 0x60, 0x0, 0x4f, 0xc8, 0x88,
    0x7, 0xff, 0xff, 0xf1,

    /* U+0033 "3" */
    0xb, 0xff, 0xb2, 0x0, 0x96, 0x9f, 0xb0, 0x0,
    0x16, 0xf9, 0x0, 0x7f, 0xfb, 0x0, 0x2, 0x49,
    0xfb, 0x0, 0x0, 0xe, 0xf0, 0x49, 0x69, 0xfc,
    0x3, 0xdf, 0xea, 0x10,

    /* U+0034 "4" */
    0x0, 0x6, 0xf9, 0x0, 0x1, 0xef, 0x90, 0x0,
    0xbc, 0xf9, 0x0, 0x8e, 0x3f, 0x90, 0x6f, 0x52,
    0xf9, 0xc, 0xff, 0xff, 0xf9, 0x34, 0x45, 0xfb,
    0x20, 0x0, 0x1f, 0x90,

    /* U+0035 "5" */
    0xc, 0xff, 0xfb, 0x0, 0xdd, 0x77, 0x50, 0xe,
    0xa0, 0x0, 0x0, 0xff, 0xfe, 0x40, 0x5, 0x47,
    0xfe, 0x0, 0x0, 0xd, 0xf0, 0x29, 0x69, 0xfc,
    0x1, 0xdf, 0xea, 0x10,

    /* U+0036 "6" */
    0x0, 0x7e, 0xfc, 0x0, 0x8f, 0xb6, 0x70, 0x1f,
    0xb0, 0x0, 0x5, 0xfb, 0xef, 0xa0, 0x6f, 0xd4,
    0xbf, 0x54, 0xf8, 0x5, 0xf7, 0xe, 0xe7, 0xcf,
    0x20, 0x2c, 0xfd, 0x50,

    /* U+0037 "7" */
    0x7f, 0xff, 0xff, 0x63, 0x77, 0x7d, 0xf2, 0x0,
    0x2, 0xf9, 0x0, 0x0, 0xbf, 0x10, 0x0, 0x3f,
    0xa0, 0x0, 0x9, 0xf4, 0x0, 0x0, 0xef, 0x0,
    0x0, 0x2f, 0xb0, 0x0,

    /* U+0038 "8" */
    0x5, 0xdf, 0xd5, 0x1, 0xfc, 0x5d, 0xf1, 0x1f,
    0xb2, 0xbe, 0x0, 0x5f, 0xff, 0x40, 0x2f, 0xb5,
    0xce, 0x17, 0xf4, 0x5, 0xf6, 0x4f, 0xb5, 0xcf,
    0x30, 0x6d, 0xfd, 0x50,

    /* U+0039 "9" */
    0x5, 0xdf, 0xb2, 0x3, 0xfc, 0x7e, 0xd0, 0x7f,
    0x40, 0x8f, 0x36, 0xf9, 0x2c, 0xf5, 0xc, 0xff,
    0xdf, 0x40, 0x3, 0x2a, 0xf1, 0x8, 0x6a, 0xf9,
    0x0, 0xdf, 0xe8, 0x0,

    /* U+00B0 "°" */
    0xa, 0xd7, 0x4, 0xb1, 0xf0, 0xb, 0xe7, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 78, .box_w = 5, .box_h = 3, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 8, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 36, .adv_w = 110, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 52, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 80, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 108, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 136, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 164, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 192, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 220, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 248, .adv_w = 110, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 276, .adv_w = 73, .box_w = 5, .box_h = 3, .ofs_x = 0, .ofs_y = 5}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9,
    0xa, 0xb, 0xc, 0x83
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 45, .range_length = 132, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 12, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t alphapi_num_12 = {
#else
lv_font_t alphapi_num_12 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 8,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if ALPHAPI_NUM_12*/

