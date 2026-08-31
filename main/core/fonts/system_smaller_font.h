/*******************************************************************************
 * system_smaller_font.h
 * 系统小号字体（MiSans-Bold 20px，bpp 2，含常用中文 + ASCII）。
 * 由 lv_font_conv 生成在 system_smaller_font.c，此处仅导出字体描述符声明。
 *
 * 注意：本字体不包含 LVGL 内置的 PUA 区图标字形（LV_SYMBOL_*，U+F001~F8FF），
 * 使用这些图标的控件会显示为占位符方块。
 ******************************************************************************/
#ifndef SYSTEM_SMALLER_FONT_H
#define SYSTEM_SMALLER_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* MiSans-Bold 20px，系统小号字体 */
extern const lv_font_t system_smaller_font;

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SMALLER_FONT_H */
