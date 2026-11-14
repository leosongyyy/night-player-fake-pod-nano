#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file quiet_mode.h
 * @brief 安静模式（蓝牙不断电的"伪关机"，2026-11）
 *
 * 场景：睡前用蓝牙耳机听歌，睡眠定时到点后如果**直接真关机**，耳机侧会
 * 因为链路断开播放提示音/闪灯，反而吵醒人。安静模式做的是：
 *
 *   - 停止播放（音量渐降后由睡眠定时器触发进入）；
 *   - 关屏幕（AMOLED SLEEP IN）+ 挂起 LVGL + 关 DAC/耳放 + 卸载 SD；
 *   - **保留蓝牙连接**（只把 A2DP 媒体流挂起，不断链路）；
 *   - CPU 固定 80MHz，进入"只有蓝牙还活着"的状态。
 *
 * 退出条件（任一即 **AXP2101 真断电**，无提示界面）：
 *   - 蓝牙链路断开（耳机被拿走/没电/关机）；
 *   - 持续超过 **12 小时**；
 *   - 电池掉到 **15% 以下**（充电中不算）。
 *
 * 用户主动退出：**长按电源键** → 重启进正常系统（短按无反应，防误触）。
 *
 * 前提：只有"当前连接的蓝牙设备在 bt_store 里开启了 quiet_mode"才会进入
 * （开关绑定在设备设置页，默认关）。睡眠定时到点时由 sleep_timer 调用
 * quiet_mode_try_enter() 判定。
 *
 * 线程模型：
 *   - quiet_mode_try_enter() 由睡眠定时任务调用（内部 RAM 栈）：它会先做
 *     "息屏"（power_service_screen_off，内部投递 deep_worker），再投递
 *     deep_worker 执行停播放/关外设/启动监控任务，两段不嵌套；
 *   - 监控任务栈在 PSRAM（只读 I2C/写 PMU，不碰 flash）；
 *   - 任何任务可查询 quiet_mode_is_active()。
 */

/** 初始化（当前实现无需资源，保留给 main 统一调用；幂等） */
void quiet_mode_init(void);

/**
 * @brief 若条件满足则进入安静模式（睡眠定时到点调用）
 *
 * @return true  = 已进入（或本来就在安静模式），调用方**不要**再走关机流程；
 *         false = 条件不满足（未连接蓝牙/设备未开安静模式），调用方按普通关机处理。
 */
bool quiet_mode_try_enter(void);

/** @brief 当前是否处于安静模式 */
bool quiet_mode_is_active(void);

/**
 * @brief 安静模式下长按电源键：保存 NVS 后重启进正常系统
 *
 * 由 key_handler 在安静模式中检测到上键长按调用。不返回（esp_restart）。
 */
void quiet_mode_request_reboot(void);

#ifdef __cplusplus
}
#endif
