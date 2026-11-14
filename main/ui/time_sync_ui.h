#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file time_sync_ui.h
 * @brief 「WiFi对时」全屏进度界面（WiFi + NTP）
 *
 * 点击设置菜单 → 时间与日期 → WiFi对时 后：
 *   1. 在当前 LVGL 任务里创建全屏进度层（黑色不透明），并置 hold_awake
 *      抑制自动息屏；
 *   2. 起一个**内部 RAM 栈**任务执行 core/network/wifi_time_sync（WiFi 初始化
 *      会写 NVS/PHY 校准数据，PSRAM 栈任务不能碰关缓存段）；
 *   3. 进度/阶段文字由同步任务经 lvgl_port_lock 回写 UI；
 *   4. 完成/失败后显示结果，成功后 2.5 秒自动关闭，失败则等用户点「关闭」；
 *      进度期间「取消」置取消标志，同步任务 100ms 内响应并清理 WiFi。
 *
 * ⚠ 全部 UI 接口须在 LVGL 任务/持锁上下文调用（time_sync_ui_start 由菜单
 *   点击回调直接调用即可）。
 */

/** @brief 开始一次网络对时（已在运行/已显示时忽略） */
void time_sync_ui_start(void);

/** @brief 全屏对时界面是否正在显示（不含已关闭） */
bool time_sync_ui_is_visible(void);

/** @brief 取消当前对时（等价于点界面上的「取消」；无对时在跑时无操作） */
void time_sync_ui_cancel(void);

#ifdef __cplusplus
}
#endif
