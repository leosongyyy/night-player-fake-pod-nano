#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fake_shutdown.h
 * @brief 关机流程（S31：AXP2101 软关机，替代 S3 的 deepsleep 伪关机）
 *
 * 流程：显示「正在关机...」→ 保存播放现场/NVS → 停播放 → **优雅关断音频
 *       （CS43131 pop-free 掉电，防关机爆音）** → 挂起 LVGL → 卸载 SD →
 *       补足提示的最少可见时长（0.5s）→ 写 AXP2101 `0x10[0]=1` 真断电。
 *
 * ⚠ 提示界面按"最少可见 0.5s"处理（见 fake_shutdown.c 的 FS_UI_MIN_SHOW_MS）：
 *   保存 NVS/卸载 SD 的耗时随数据量波动，没有下限时关机提示会"一闪就没"。
 * ⚠ 音频必须在本流程里优雅关断：PMU 是一刀切断电，DAC/耳放仍在工作状态时
 *   直接掉电会在耳机里炸出一声爆音（关机越快越明显）。没在播放时 DAC 同样
 *   是上电的，所以不能只依赖"停播放"。
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

/**
 * @brief 充电专用界面里"拔掉充电器"触发的关机
 *
 * 与普通关机走同一条流程（deep_worker 大栈执行、挂起 LVGL、写 PMU 0x10），
 * 只是提示文案改为「充电器已断开，正在关机...」。
 *
 * ⚠ 专用于 boot_flow 的充电界面（插电开机）；该状态下播放器/SD/NVS 都未
 *   初始化，流程里的保存现场/停播放/卸 SD 都是无操作（各函数内部有 ready
 *   判断），真正生效的是"挂起 LVGL + PMU 断电"。
 * ⚠ 与其它入口共用互斥标志；失败后不会自动重试（PMU 长按 4s 硬件兜底）。
 */
void fake_shutdown_charger_unplugged(void);

#ifdef __cplusplus
}
#endif
