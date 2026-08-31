/*******************************************************************************
 * system_font.h
 * 系统字体引用：统一从这里取"系统默认字体"，不要在 UI 里直接引用某个具体字体。
 *
 * 通过 Kconfig CONFIG_UI_SYSTEM_FONT_FULL 决定引用指向：
 *   - 开启：完整版 system_global_font（字全、体积大、烧录慢，正常使用）
 *   - 关闭：精简版 system_global_font_lite（仅系统 UI 用字 + ASCII，体积小、
 *           烧录快，适合调试阶段）
 *
 * 后续新增文字一律使用 UI_SYSTEM_FONT 宏，便于一键切换字体以加快调试烧录。
 ******************************************************************************/
#ifndef SYSTEM_FONT_H
#define SYSTEM_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "system_global_font.h"
#include "system_global_font_lite.h"

#ifdef CONFIG_UI_SYSTEM_FONT_FULL
#define UI_SYSTEM_FONT (&system_global_font)
#else
#define UI_SYSTEM_FONT (&system_global_font_lite)
#endif

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_FONT_H */
