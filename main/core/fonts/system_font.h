/*******************************************************************************
 * system_font.h
 * 系统字体统一入口：所有 UI 代码一律用 `UI_SYSTEM_FONT` 取字体，不要直接引用
 * 某个具体字体。
 *
 * 生效顺序（2026-11 起，S31 主线也改为位图字体）：
 *   1. `font` 分区里的 NPF1 全字库（Droid Sans Fallback 24px，34,461 个码位，
 *      mmap 直读；见 ideas_and_exps/NPF_FONT.md）——正常发布版走这条；
 *   2. 内嵌的精简位图字库 system_global_font_lite（24px，仅系统 UI 用字 + ASCII）——
 *      没刷字体 / 镜像校验失败时的兜底，保证开机不会出现"无字"。
 *
 * 注意：`UI_SYSTEM_FONT` 是一个**运行期取指针的函数调用**，不是常量。它可以
 * 用在任何需要 `const lv_font_t *` 的地方（样式、line_height 等），但不要在静态
 * 初始化器里使用。
 *
 * 三个字体宏的分工：
 *   - UI_SYSTEM_FONT   ：正文/列表/标题（24px 全字库，覆盖全部汉字）
 *   - UI_SMALL_FONT    ：状态栏时间/两行文字图标用的 20px **内嵌位图**字体
 *                        （system_smaller_font；NPF 只烘焙了 24px，20px 全字库要
 *                         再占 6MB 分区，而这两处只用到数字/固定文案，内嵌够用）
 *   - UI_ICON_TEXT_FONT：图标 + 文字混排（一个 label 里既有 LV_SYMBOL_* 图标又有
 *                        中文）。NPF 与 20px 内嵌字体都不含 PUA 图标字形，而
 *                        system_global_font_lite 的 `.fallback` 指向内置
 *                        lv_font_montserrat_24，LVGL 会逐字回退，图标与中文都能画。
 ******************************************************************************/
#ifndef SYSTEM_FONT_H
#define SYSTEM_FONT_H

#include <stdbool.h>

#include "lvgl.h"

#include "font_npf.h"
#include "system_global_font_lite.h"
#include "system_smaller_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统字体（开机一次，早于任何 UI 创建）
 *
 * 尝试挂载 font 分区的 NPF1 全字库；失败时自动回退内嵌精简字库并打印警告。
 * ⚠ 只做"分区查找 + mmap + 填结构体"，不碰 LVGL 对象，可在 LVGL 锁外调用。
 */
void system_font_init(void);

/**
 * @brief 取当前生效的系统字体
 *
 * @return 永远非 NULL：NPF1 全字库（已挂载）或内嵌精简字库
 */
const lv_font_t *system_font_get(void);

/** 当前是否在用外置 NPF1 全字库（false = 回退内嵌精简字库） */
bool system_font_is_full(void);

/** UI 取字体的唯一宏 */
#define UI_SYSTEM_FONT (system_font_get())

/** 20px 内嵌位图小字体（数字/固定双行文字图标；见文件头说明） */
#define UI_SMALL_FONT  (&system_smaller_font)

/** 图标 + 文字混排字体（LV_SYMBOL_* + 中文；见文件头说明） */
#define UI_ICON_TEXT_FONT  (&system_global_font_lite)

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_FONT_H */
