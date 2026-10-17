/*******************************************************************************
 * system_font.h
 * 系统字体引用：统一从这里取"系统默认字体"，不要在 UI 里直接引用某个具体字体。
 *
 * 2026-10 起系统字体是 **font 分区里的 `system.ttf`**（FreeType 按需光栅化；当前内容
 * Droid Sans Fallback，路径由 CMake 写死为 spiffs/system.ttf），
 * 由 core/fonts/font_ttf.c 提供，详见 ideas_and_exps/FontSystem.md：
 *   - UI_SYSTEM_FONT：正文/列表/标题用的 24px 系统字体（覆盖全部汉字）
 *   - UI_SMALL_FONT ：状态栏/时间/两行文字图标用的 20px 字体
 *
 * ⚠ 这两个宏现在是**运行时函数调用**（不是常量指针）：
 *   · 不要用在静态初始化里（如 `static const lv_font_t *f = UI_SYSTEM_FONT;`）；
 *   · TTF 不可用时（font 分区为空/文件损坏/CONFIG_UI_FONT_TTF=n）会自动返回
 *     内嵌位图回退字体，所以任何情况下都能直接用，UI 不会没字。
 *   · 字号没预创建时会返回回退字体并打印一条 W 级日志（见 font_ttf_get）。
 *
 * 历史（迁移前）：字体是编译进固件的位图字库，由 CONFIG_UI_SYSTEM_FONT_FULL
 * 在"完整版 system_global_font（2.4MB，行高 33）"与"精简版
 * system_global_font_lite（行高 26）"之间切换；精简版只含系统 UI 用字 + ASCII，
 * 音乐文件名里的生僻字会显示成空白。这两套字库现在只剩**回退**作用：
 * system_global_font_lite 始终编译，完整版只在 CONFIG_UI_FONT_TTF=n 时才有意义。
 *
 * ⚠ 更早还试过把完整字库放 SPIFFS 运行时加载（font_store/lv_font_load），实测
 *   解析 2.4MB 要 8 秒（lv_font_loader 逐字节解析）开机太慢，已改回编译进固件；
 *   这就是本次改用 TTF + FreeType 的动机之一。
 *
 * 后续新增文字一律使用 UI_SYSTEM_FONT / UI_SMALL_FONT 宏，便于统一换字体；
 * 需要"图标 + 文字"混排（含 LV_SYMBOL_*）时用 UI_ICON_TEXT_FONT（见下方说明）。
 ******************************************************************************/
#ifndef SYSTEM_FONT_H
#define SYSTEM_FONT_H

#include "sdkconfig.h"

#include "lvgl.h"

#include "core/fonts/font_ttf.h"
#include "system_global_font.h"
#include "system_global_font_lite.h"
#include "system_smaller_font.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_UI_FONT_TTF
/* 主路径：TTF（FreeType） */
#define UI_SYSTEM_FONT (font_ttf_get(UI_FONT_PX_MAIN))
#define UI_SMALL_FONT  (font_ttf_get(UI_FONT_PX_SMALL))
#else
/* 编译期关掉 TTF：回到内嵌位图字体（完整版/精简版由 Kconfig 决定） */
#ifdef CONFIG_UI_SYSTEM_FONT_FULL
#define UI_SYSTEM_FONT (&system_global_font)
#else
#define UI_SYSTEM_FONT (&system_global_font_lite)
#endif
#define UI_SMALL_FONT  (&system_smaller_font)
#endif

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
