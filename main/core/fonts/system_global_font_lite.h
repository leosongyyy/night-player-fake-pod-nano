/*******************************************************************************
 * system_global_font_lite.h
 * 系统全局字体精简版（MiSans-Normal 24px，bpp 2，仅含系统 UI 所需中文 + ASCII）。
 * 由 lv_font_conv 生成在 system_global_font_lite.c，此处仅导出字体描述符声明。
 *
 * 用途：内嵌位图**回退字体**——TTF（font 分区 + FreeType）打不开时顶替，
 *       保证界面仍有字。完整版字库已随旧的"font 维度"移除，本文件现在
 *       始终参与编译（见 core/fonts/font_ttf.c 的 fallback_font_for_px）。
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
