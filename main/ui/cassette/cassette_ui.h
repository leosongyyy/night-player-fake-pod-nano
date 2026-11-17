#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_ui.h
 * @brief 袖珍磁带机 UI 总入口（由 boot_enter_normal() 一处调用）
 *
 * 容器分层（全部在 lv_scr_act() 屏幕树）：
 *   cas_root  —— 全屏不透明底，保证无残影
 *     shell   —— 机箱（渐变底 + 双层金属框 + 四角螺丝）+ 状态栏，常驻
 *     pages   —— 7 个全屏页根，同一时刻仅 1 个非 HIDDEN
 *     drawer  —— 全屏不透明抽屉，最后创建（最上）
 */

/** 创建整套新 UI；必须在 LVGL 任务上下文/持锁时调用 */
void cassette_ui_create(void);

/** 状态栏中央标题随当前模块同步（由 cas_nav_show 调用） */
void cas_shell_set_title(const char *title);

#ifdef __cplusplus
}
#endif
