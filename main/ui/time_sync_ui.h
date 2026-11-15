#pragma once

#include <stdbool.h>

#include "core/network/wifi_time_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file time_sync_ui.h
 * @brief 「WiFi对时」全屏进度界面（WiFi + NTP）
 *
 * 2026-11 起这是**里世界固件**（ota_0，矩阵 role=inner）里的界面之一，表世界
 * 不再直接用它：表世界点「WiFi对时」只做两件事 —— 弹统一确认窗、把下次启动
 * 切到 ota_0 并重启。重启后由 `core/system/inner_world_app.c` 初始化最小系统
 * 与里世界功能列表，用户点「WiFi对时」才调用本模块：
 *   1. 在 LVGL 任务里创建全屏进度层（黑色不透明）；
 *   2. 起一个**内部 RAM 栈**任务执行 core/network/wifi_time_sync（WiFi 初始化
 *      会写 NVS/PHY 校准数据，PSRAM 栈任务不能碰关缓存段）；
 *   3. 进度/阶段文字由同步任务经 lvgl_port_lock 回写 UI；
 *   4. 完成后显示结果，再按 `time_sync_ui_set_done_cb()` 注册的位置走：
 *      · 里世界：结果展示 2.5s（失败 8s）后回调 → 回调里隐藏本界面、回功能
 *        列表并记录结果；用户点「关闭」也会立即回调。回调**不会**被清掉，
 *        里世界里可以反复对时；
 *      · 没注册回调（表世界角色，理论上不再走到这里）：成功 2.5s 自动关闭，
 *        失败等用户点「关闭」。
 *   进度期间「取消」置取消标志，同步任务 100ms 内响应并清理 WiFi。
 *
 * ⚠ 全部 UI 接口须在 LVGL 任务/持锁上下文调用。
 */

/** @brief 一次对时结束（结果已展示完毕/用户已点关闭）时的去向回调 */
typedef void (*time_sync_ui_done_cb_t)(wifi_time_sync_result_t result);

/**
 * @brief 注册"结果回调"（里世界用；在 time_sync_ui_start 之前调用）
 *
 * 注册后界面**不会自己关闭**：结果展示到期（或用户点「关闭」）时回调一次，
 * 由回调决定去向（里世界里是"隐藏本界面 + 回功能列表 + 记录结果"）。
 * 传 NULL 恢复旧行为（关闭界面回原界面）。
 * 回调在 LVGL 任务上下文执行；注册**不会被自动清除**，因此可以反复对时
 * （每次结束都回调一次）。
 */
void time_sync_ui_set_done_cb(time_sync_ui_done_cb_t cb);

/**
 * @brief 开始一次网络对时
 *
 * @return true 已开始（或本来就在显示/运行中，不会重复起任务）；
 *         false 起任务失败（内存不足，界面上会提示）——调用方应据此决定
 *               要不要退回上一级（里世界里是 Toast 后留在列表）。
 */
bool time_sync_ui_start(void);

/** @brief 全屏对时界面是否正在显示（不含已关闭） */
bool time_sync_ui_is_visible(void);

/**
 * @brief 强制隐藏对时界面（不取消正在跑的任务）
 *
 * 给「里世界」用：对时结束回调里先隐藏这一层，再显示里世界的功能列表
 * （本界面是屏幕层的全屏不透明层，不隐藏会一直盖在列表上面）。
 * 已经隐藏时无操作。
 */
void time_sync_ui_dismiss(void);

/** @brief 取消当前对时（等价于点界面上的「取消」；无对时在跑时无操作） */
void time_sync_ui_cancel(void);

#ifdef __cplusplus
}
#endif
