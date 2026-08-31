/*******************************************************************************
 * system_global_font_lite.h
 * 系统全局字体精简版（MiSans-Normal 24px，bpp 2，仅含系统 UI 所需中文 + ASCII）。
 * 由 lv_font_conv 生成在 system_global_font_lite.c，此处仅导出字体描述符声明。
 *
 * 用途：作为完整版 system_global_font 的轻量替代（体积小、烧录快），
 *       用于调试阶段。通过 Kconfig CONFIG_UI_SYSTEM_FONT_FULL 切换
 *       字体引用 UI_SYSTEM_FONT 指向完整版还是精简版。
 *
 * 注意：本字体不包含 LVGL 内置的 PUA 区图标字形（LV_SYMBOL_*，U+F001~F8FF）。
 ******************************************************************************/
#ifndef SYSTEM_GLOBAL_FONT_LITE_H
#define SYSTEM_GLOBAL_FONT_LITE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* MiSans-Normal 24px，系统全局字体精简版 */
extern const lv_font_t system_global_font_lite;

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_GLOBAL_FONT_LITE_H */
