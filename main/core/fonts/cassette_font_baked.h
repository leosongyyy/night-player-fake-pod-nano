#ifndef CASSETTE_FONT_BAKED_H
#define CASSETTE_FONT_BAKED_H
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const lv_font_t cas_font_sym;   /* DejaVu Sans: ·■▲►◄⚙❚ */
extern const lv_font_t cas_font_cn;    /* fallback -> cas_font_sym */
#define CASSETTE_FONT_BAKED_HAS_MONO 1
extern const lv_font_t cas_font_mono;
#ifdef __cplusplus
}
#endif
#endif /* CASSETTE_FONT_BAKED_H */
