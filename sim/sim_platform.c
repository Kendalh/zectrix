/* Host implementations of the few ESP/LVGL runtime calls the renderers use. */
#include "lvgl.h"
#include "esp_timer.h"

static int64_t g_now_us = 1000000;
int64_t esp_timer_get_time(void) { return g_now_us; }
void sim_advance_time_us(int64_t us) { g_now_us += us; }

static uint32_t glyph_id_for(const lv_font_fmt_txt_dsc_t* fdsc, uint32_t letter) {
    if (letter == 0) return 0;
    for (unsigned i = 0; i < fdsc->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t* c = &fdsc->cmaps[i];
        if (letter < c->range_start) continue;
        uint32_t rcp = letter - c->range_start;
        if (rcp >= c->range_length) continue;
        switch (c->type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                return c->glyph_id_start + rcp;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
                return c->glyph_id_start + ((const uint8_t*)c->glyph_id_ofs_list)[rcp];
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
                for (unsigned k = 0; k < c->list_length; k++) {
                    if (c->unicode_list[k] != rcp) continue;
                    if (c->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) return c->glyph_id_start + k;
                    return c->glyph_id_start + ((const uint16_t*)c->glyph_id_ofs_list)[k];
                }
                break;
        }
    }
    return 0;
}

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out,
                                   uint32_t letter, uint32_t letter_next) {
    (void)letter_next; /* kerning ignored: sub-pixel differences only */
    const lv_font_fmt_txt_dsc_t* fdsc = (const lv_font_fmt_txt_dsc_t*)font->dsc;
    uint32_t gid = glyph_id_for(fdsc, letter);
    if (gid == 0) return false;
    const lv_font_fmt_txt_glyph_dsc_t* g = &fdsc->glyph_dsc[gid];
    dsc_out->adv_w = (uint16_t)((g->adv_w + 8) >> 4);
    dsc_out->box_h = g->box_h;
    dsc_out->box_w = g->box_w;
    dsc_out->ofs_x = g->ofs_x;
    dsc_out->ofs_y = g->ofs_y;
    dsc_out->format = (lv_font_glyph_format_t)(fdsc->bpp == 1 ? LV_FONT_GLYPH_FORMAT_A1 : fdsc->bpp);
    dsc_out->stride = 0; /* bits are packed contiguously; rawdraw reads row_bits = box_w when stride == 0 */
    dsc_out->resolved_font = font;
    dsc_out->gid.index = gid;
    return true;
}

const void* lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t* g_dsc, struct _lv_draw_buf_t* draw_buf) {
    (void)draw_buf;
    const lv_font_t* font = g_dsc->resolved_font;
    const lv_font_fmt_txt_dsc_t* fdsc = (const lv_font_fmt_txt_dsc_t*)font->dsc;
    if (fdsc->bitmap_format != LV_FONT_FMT_TXT_PLAIN || !g_dsc->req_raw_bitmap) return NULL;
    return &fdsc->glyph_bitmap[fdsc->glyph_dsc[g_dsc->gid.index].bitmap_index];
}
