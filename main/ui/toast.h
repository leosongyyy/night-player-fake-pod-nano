#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file toast.h
 * @brief 全局 Toast 提示（类 Android，见 ideas_and_exps/FUTURE_PLAN.md 第 13 点）
 *
 * 屏幕底部弹出的短暂消息条：灰色圆角底色 + 白色文字，右侧带一个小的黄色
 * 环形倒计时，展示当前 Toast 还剩多久自动隐藏；到点后向下滑出收回。
 * 若显示期间有新的请求到达，则直接更新文字并重置 2 秒倒计时（不重复弹出）。
 *
 * 特性：
 *   - 挂在 lv_layer_top()（LVGL 系统顶层）：位于主界面/全屏设置菜单/通知栏
 *     等一切普通对象之上，图层最高；
 *   - 不接受点击事件：清除 CLICKABLE 并挂 LV_EVENT_HIT_TEST 透传，Toast
 *     区域内的触摸会落到下层控件，不挡操作；
 *   - 文本内部复制到模块内静态缓冲，调用方可使用栈上临时缓冲区。
 *
 * ⚠ 线程安全：LVGL API 非线程安全，调用方必须持有 LVGL 锁
 *   （lvgl_port_lock / lvgl_port_unlock）；LVGL 任务上下文（事件回调、
 *   lv_timer 回调）内已持锁，可直接调用。
 */

/**
 * @brief 显示 Toast（替换当前内容并重置隐藏倒计时）
 *
 * 首次调用时懒创建控件（挂 lv_layer_top）。当前未显示 → 从底部滑入；
 * 正在显示 → 原地更新文字与倒计时；正在滑出（即将隐藏）→ 拉回重新显示。
 *
 * @param text 提示文本（UTF-8；内部复制，超长部分截断）
 */
void ui_toast_show(const char *text);

/**
 * @brief 当前是否有 Toast 正在显示（含正在滑出阶段）
 * @return true = 显示中
 */
bool ui_toast_is_shown(void);

#ifdef __cplusplus
}
#endif
