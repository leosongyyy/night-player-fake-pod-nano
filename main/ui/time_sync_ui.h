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
 * 2026-11-15 起这是**对时专用固件**（ota_0）里的界面，播放器不再直接用它：
 * 播放器点「WiFi对时」只做两件事 —— 把下次启动切到 ota_0、重启。重启后由
 * `core/system/sync_app.c` 初始化最小系统并调用本模块：
 *   1. 在 LVGL 任务里创建全屏进度层（黑色不透明）；
 *   2. 起一个**内部 RAM 栈**任务执行 core/network/wifi_time_sync（WiFi 初始化
 *      会写 NVS/PHY 校准数据，PSRAM 栈任务不能碰关缓存段）；
 *   3. 进度/阶段文字由同步任务经 lvgl_port_lock 回写 UI；
 *   4. 完成后显示结果，再按 `time_sync_ui_set_done_cb()` 注册的位置走：
 *      · 对时固件：结果展示 2.5s（失败 8s）后回调 → 回调里写 NVS 结果 + 重启回
 *        播放器；用户点「关闭」也会立即回调；
 *      · 没注册回调（播放器角色，理论上不再走到这里）：成功 2.5s 自动关闭，
 *        失败等用户点「关闭」。
 *   进度期间「取消」置取消标志，同步任务 100ms 内响应并清理 WiFi。
 *
 * ⚠ 全部 UI 接口须在 LVGL 任务/持锁上下文调用。
 */

/** @brief 一次对时结束（结果已展示完毕/用户已点关闭）时的去向回调 */
typedef void (*time_sync_ui_done_cb_t)(wifi_time_sync_result_t result);

/**
 * @brief 注册"结果回调"（对时固件用；在 time_sync_ui_start 之前调用）
 *
 * 注册后界面**不会自己关闭**：结果展示到期（或用户点「关闭」）时回调一次，
 * 由回调决定去向（对时固件里是"写结果 + esp_restart() 回播放器"）。
 * 传 NULL 恢复旧行为（关闭界面回原界面）。
 * 回调只在 LVGL 任务上下文执行一次（内部会先把注册清掉）。
 */
void time_sync_ui_set_done_cb(time_sync_ui_done_cb_t cb);

/**
 * @brief 开始一次网络对时
 *
 * @return true 已开始（或本来就在显示/运行中，不会重复起任务）；
 *         false 起任务失败（内存不足，界面上会提示）——调用方应据此决定
 *               要不要退回上一级（对时固件里是"直接重启回播放器"）。
 */
bool time_sync_ui_start(void);

/** @brief 全屏对时界面是否正在显示（不含已关闭） */
bool time_sync_ui_is_visible(void);

/** @brief 取消当前对时（等价于点界面上的「取消」；无对时在跑时无操作） */
void time_sync_ui_cancel(void);

#ifdef __cplusplus
}
#endif
