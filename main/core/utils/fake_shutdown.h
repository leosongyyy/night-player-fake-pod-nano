#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fake_shutdown.h
 * @brief 关机流程（S31：AXP2101 软关机，替代 S3 的 deepsleep 伪关机）
 *
 * 流程：显示「正在关机...」→ 保存播放现场/NVS → 停播放 → 挂起 LVGL →
 *       卸载 SD → 写 AXP2101 `0x10[0]=1` 真断电。
 *
 * ⚠ 不再显示 S3 时代的「您现在可以安全断开电源了」界面：PMU 会自己断电，
 *   不需要用户手动拔电源。若 PMU 未响应（2.5s 后仍存活），屏幕会提示
 *   "关机失败，请长按电源键 4 秒"，由 PMU 的 OFFLEVEL 硬件兜底断电。
 * ⚠ 入口可被任意任务调用（PMU 事件任务 / 睡眠定时器），**不可在
 *   deep_worker 执行体内调用**（本模块自身会投递 deep_worker，嵌套会死锁）。
 * ⚠ 流程进行中重复调用会被忽略（互斥标志）。
 */

/**
 * @brief 普通关机（睡眠定时器/低电等自动策略调用）
 *
 * 与电源键长按走同一条流程；失败只打印日志，不影响开机。
 */
void fake_shutdown_enter(void);

/**
 * @brief 电源键长按关机（PMU 按键事件调用）
 *
 * 与 fake_shutdown_enter 同一条流程，仅日志措辞不同。
 */
void fake_shutdown_wait_power_off(void);

#ifdef __cplusplus
}
#endif
