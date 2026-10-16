/* PC simulator stand-in for LVGL: only the font types the generated font .c files need. */
#ifndef SIM_LVGL_H
#define SIM_LVGL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rawdraw/font_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LV_ATTRIBUTE_LARGE_CONST
#define LV_FONT_FMT_TXT_LARGE 0
#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 3
#define LV_FONT_SUBPX_NONE 0
#define LV_VERSION_CHECK(x, y, z) 1
#define LV_USE_FONT_PLACEHOLDER 1

typedef struct {
    uint32_t bitmap_index : 20;
    uint32_t adv_w : 12;
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;
} lv_font_fmt_txt_glyph_dsc_t;

typedef enum {
    LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL,
    LV_FONT_FMT_TXT_CMAP_SPARSE_FULL,
    LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,
} lv_font_fmt_txt_cmap_type_t;

typedef struct {
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    const uint16_t* unicode_list;
    const void* glyph_id_ofs_list;
    uint16_t list_length;
    lv_font_fmt_txt_cmap_type_t type;
} lv_font_fmt_txt_cmap_t;

typedef struct {
    const void* glyph_ids;
    const int8_t* values;
    uint32_t pair_cnt : 30;
    uint32_t glyph_ids_size : 2;
} lv_font_fmt_txt_kern_pair_t;

typedef struct {
    const int8_t* class_pair_values;
    const uint8_t* left_class_mapping;
    const uint8_t* right_class_mapping;
    uint8_t left_class_cnt;
    uint8_t right_class_cnt;
} lv_font_fmt_txt_kern_classes_t;

typedef enum {
    LV_FONT_FMT_TXT_PLAIN = 0,
    LV_FONT_FMT_TXT_COMPRESSED = 1,
    LV_FONT_FMT_TXT_COMPRESSED_NO_PREFILTER = 2,
} lv_font_fmt_txt_bitmap_format_t;

typedef struct {
    uint32_t last_letter;
    uint32_t last_glyph_id;
} lv_font_fmt_txt_glyph_cache_t;

typedef struct {
    const uint8_t* glyph_bitmap;
    const lv_font_fmt_txt_glyph_dsc_t* glyph_dsc;
    const lv_font_fmt_txt_cmap_t* cmaps;
    const void* kern_dsc;
    uint16_t kern_scale;
    uint16_t cmap_num : 9;
    uint16_t bpp : 4;
    uint16_t kern_classes : 1;
    uint16_t bitmap_format : 2;
    uint8_t stride;
} lv_font_fmt_txt_dsc_t;

const void* lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t* g_dsc, struct _lv_draw_buf_t* draw_buf);
bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out,
                                   uint32_t unicode_letter, uint32_t unicode_letter_next);

#ifdef __cplusplus
}
#endif
#endif
