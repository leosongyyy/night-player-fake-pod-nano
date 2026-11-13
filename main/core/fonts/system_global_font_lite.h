/*******************************************************************************
 * system_global_font_lite.h
 * 系统全局字体精简版（MiSans-Normal 24px，bpp 2，仅含系统 UI 所需中文 + ASCII）。
 * 由 lv_font_conv 生成在 system_global_font_lite.c，此处仅导出字体描述符声明。
 *
 * 用途：内嵌位图**回退字体**——font 分区的 NPF1 全字库（运行时 mmap）挂载失败
 *       （没刷字体 / 镜像损坏 / 分区缺失）时顶替，保证界面仍有字。本文件始终参与
 *       编译，由 core/fonts/system_font.c 选中（见 system_font_get()）。
 *       它同时是 UI_ICON_TEXT_FONT：`.fallback` 指向 lv_font_montserrat_24，
 *       "图标 + 中文"混排靠 LVGL 逐字回退。
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
