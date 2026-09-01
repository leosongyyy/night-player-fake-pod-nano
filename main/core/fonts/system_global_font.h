/*******************************************************************************
 * system_global_font.h
 * 系统全局默认字体（MiSans-Normal 24px，bpp 2，含常用中文/符号）。
 * 由 lv_font_conv 生成在 system_global_font.c，此处仅导出字体描述符声明。
 *
 * 注意：本字体不包含 LVGL 内置的 PUA 区图标字形（LV_SYMBOL_*，U+F001~F8FF），
 * 使用这些图标的控件会显示为占位符方块。若需要图标，需用 lv_font_conv 重新
 * 生成并把 --symbols 加上对应 PUA 码位。
 ******************************************************************************/
#ifndef SYSTEM_GLOBAL_FONT_H
#define SYSTEM_GLOBAL_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* MiSans-Normal 24px，系统默认字体 */
extern const lv_font_t system_global_font;

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_GLOBAL_FONT_H */
