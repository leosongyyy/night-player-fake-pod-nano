/*******************************************************************************
 * system_font.h
 * 系统字体引用：统一从这里取"系统默认字体"，不要在 UI 里直接引用某个具体字体。
 *
 * 2026-10 起系统字体固定为 **font 分区里的 `system.ttf`**（FreeType 按需光栅化；
 * 当前内容 Droid Sans Fallback，路径由 CMake 写死为 spiffs/system.ttf），
 * 由 core/fonts/font_ttf.c 提供，详见 ideas_and_exps/FontSystem.md：
 *   - UI_SYSTEM_FONT：正文/列表/标题用的 24px 系统字体（覆盖全部汉字）
 *   - UI_SMALL_FONT ：状态栏/时间/两行文字图标用的 20px 字体
 *
 * ⚠ 这两个宏现在是**运行时函数调用**（不是常量指针）：
 *   · 不要用在静态初始化里（如 `static const lv_font_t *f = UI_SYSTEM_FONT;`）；
 *   · TTF 不可用时（font 分区为空/文件损坏）会自动返回内嵌位图回退字体，
 *     所以任何情况下都能直接用，UI 不会没字。
 *   · 字号没预创建时会返回回退字体并打印一条 W 级日志（见 font_ttf_get）。
 *
 * 回退字体：精简版 system_global_font_lite（24px，行高 26，仅系统 UI 用字 + ASCII）
 * 与 system_smaller_font（20px）**始终编译进固件**，只在 TTF 打不开时顶替；
 * 完整版字库（2.4MB）已随旧的"font 维度"一并移除。
 *
 * 后续新增文字一律使用 UI_SYSTEM_FONT / UI_SMALL_FONT 宏，便于统一换字体；
 * 需要"图标 + 文字"混排（含 LV_SYMBOL_*）时用 UI_ICON_TEXT_FONT（见下方说明）。
 ******************************************************************************/
#ifndef SYSTEM_FONT_H
#define SYSTEM_FONT_H

#include "sdkconfig.h"

#include "lvgl.h"

#include "core/fonts/font_ttf.h"
#include "system_global_font_lite.h"
#include "system_smaller_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 系统字体固定走 TTF（FreeType），没有构建期开关。 */
#define UI_SYSTEM_FONT (font_ttf_get(UI_FONT_PX_MAIN))
#define UI_SMALL_FONT  (font_ttf_get(UI_FONT_PX_SMALL))

/* 图标 + 文字混排字体（一个 label 里既有 LV_SYMBOL_* 图标又有中文，如
   「↑ 向上滑动关闭」）：
   - TTF 字体不含 PUA 区图标字形（U+F001~F8FF），图标会画成方块；
   - 内嵌位图字体（24px，始终编译）本身也不含图标，但它的 `.fallback` 指向
     LVGL 内置的 lv_font_montserrat_24，LVGL 逐字回退（lv_font_get_glyph_dsc
     的 resolved_font 机制），所以图标与中文都能画出来。
   字号与 UI_SYSTEM_FONT 相同（24px），可直接替换纯文字用的系统字体。
   ⚠ 只用于混排；纯文字/纯图标仍用 UI_SYSTEM_FONT / UI_SMALL_FONT 或
     lv_font_montserrat_*（纯图标）。 */
#define UI_ICON_TEXT_FONT  (&system_global_font_lite)

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_FONT_H */
