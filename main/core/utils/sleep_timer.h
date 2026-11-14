#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file sleep_timer.h
 * @brief 睡眠定时器（独立后台任务倒计时，到点触发伪关机）
 *
 * 设置时长（分钟）后，独立任务（Core1）每 500ms 检查一次绝对到期时间，
 * 倒计时结束调用 fake_shutdown_enter()（保存播放位置、优雅停播、关外设、
 * 进入深度休眠）。
 *
 * 音量渐降（NightPlayer 原版机制）：剩余时间 <= 30 秒时，从当前音量线性
 * 降到 0 档。渐降只写 audio 层硬件、不写 config_store，因此不会持久化——
 * 下次开机仍恢复用户原音量。取消定时或重新设置（延长）后自动恢复渐降前
 * 音量；渐降期间用户手动调音量（见 sleep_timer_fade_cancel）则用户接管，
 * 不再自动降低。
 *
 * 清醒感知（NightPlayer 原版机制，config_store 的 sleep_awake 开关控制，默认开）：
 * 睡眠定时开启期间，用户动作（切歌/播放控制/点歌等，UI 层在操作点调用
 * sleep_timer_note_activity）会触发加时：追加量 = (总时长 - 当前剩余) × 0.382
 * （0.382 = 1 - 0.618，黄金分割互补），即每次把剩余时间向总时长拉回 38.2%——
 * 剩余越少追加越多，连续触发逐次趋近总时长。原版无冷却：连续按键说明用户
 * 必然清醒，每次动作都立即追加。⚠ 音量调节不再加时（调音量不代表清醒，
 * 仅切歌/播放控制/点歌等"听觉上仍在活动"的动作算用户动作）。
 *
 * 注意：定时到点进入 deepsleep 后系统重启，本模块状态不保留；开机默认
 * 无定时（关闭）。开机是否自动开启由 config_store 的 sleep_auto 决定
 * （总是关闭 / 按时间段 / 总是开启，判定见 sleep_timer_boot_autostart_min）；
 * 开机自动开启时，进入主界面后由 boot_flow 弹 Toast 提醒用户"定时已开启"，
 * 便于用户主动关闭（见 ideas_and_exps/FUTURE_PLAN.md 第 27 点）。
 */

/**
 * @brief 初始化睡眠定时器（创建倒计时任务，可重复调用）
 */
void sleep_timer_init(void);

/**
 * @brief 开机默认应自动开启的睡眠定时分钟数（纯查询，无副作用）
 *
 * 读取 config_store 的 sleep_auto 模式、时间段（start/end 小时）与
 * sleep_default_min（默认时长，默认 45 分钟）：
 *   - 总是关闭（OFF）→ 0（开机不自动开启）；
 *   - 按时间段（RANGE）→ 当前 RTC 小时落在 [start, end) 内才继续判定
 *     （支持跨零点：start > end 时视为 [start, 24) ∪ [0, end)，
 *     例如 22 → 6 表示 22:00~05:59；start == end 为非法区间，不开启）；
 *     不在时间段内 → 0；RTC 不可用 → 0（无法判断，保守不开启）；
 *   - 总是开启（ALWAYS）→ 继续判定；
 *   - 以上任一"继续判定"分支若默认时长为 0 → 0；
 *   - 否则返回默认时长（>0）。
 *
 * 两处调用方共用本函数，保证判定一致：
 *   - main.c 开机时按返回值调用 sleep_timer_set() 真正启动定时；
 *   - boot_flow 进入主界面后按返回值弹 Toast 提醒用户"定时已开启"
 *     （见 ideas_and_exps/FUTURE_PLAN.md 第 27 点）。
 *
 * ⚠ 模式为"总是关闭"时，**任何自动路径都不得启动定时**：开机不启动
 *   （main.c）；切换配置槽位也完全不动定时（睡眠定时与配置槽位互相独立，
 *   时长/模式/时间段都是全局配置，见 config_slots.c）。记忆的时长只是
 *   "下次开启时用的默认档位"。
 *
 * @return >0 = 开机应自动开启该分钟数的定时；0 = 不自动开启
 */
uint32_t sleep_timer_boot_autostart_min(void);

/**
 * @brief 设置/取消睡眠定时
 * @param minutes 分钟数；0 = 取消定时；>0 = 从现在起倒计时 minutes 分钟
 *
 * 若当前正在音量渐降（最后 30 秒），取消或重新设置都会触发"恢复渐降前
 * 音量"（由倒计时任务在下一周期统一执行）。
 */
void sleep_timer_set(uint32_t minutes);

/** @brief 当前定时分钟数（0 = 未定时）。返回设置值，不含已流逝部分 */
uint32_t sleep_timer_get_minutes(void);

/**
 * @brief 当前剩余时间（秒），供 UI 显示"分:秒"
 * @return 剩余秒数（向上取整）；未定时返回 0
 */
uint32_t sleep_timer_get_remaining_s(void);

/** @brief 是否已开启睡眠定时 */
bool sleep_timer_is_active(void);

/** @brief 是否正在音量渐降（睡眠定时最后 30 秒） */
bool sleep_timer_fade_is_active(void);

/**
 * @brief 静默取消音量渐降（不恢复音量）
 *
 * 渐降期间用户手动调节音量时调用：取消自动渐降，以用户设置的新音量为准，
 * 不再继续降低（区别于取消/延长定时时的"恢复渐降前音量"）。
 */
void sleep_timer_fade_cancel(void);

/**
 * @brief 清醒感知：用户动作（切歌/播放控制/点歌等）触发睡眠定时加时
 *
 * NightPlayer 原版算法：追加量 = (总时长 - 当前剩余时间) × 0.382
 * （0.382 = 1 - 0.618，黄金分割互补），把剩余时间向总时长拉回 38.2%，
 * 无冷却（连续操作说明用户必然清醒，每次都立即追加）。
 * 仅在定时开启且清醒感知开启（config_store 的 sleep_awake）时生效；
 * 由 UI 层在用户主动操作点调用（息屏按键播放控制、播放页控制、
 * 列表选歌等）。⚠ 音量调节（按键/通知栏滑块）不再调用本函数加时。
 */
void sleep_timer_note_activity(void);

/**
 * @brief 释放睡眠定时任务（仅"重新扫描/重启前腾内部 RAM"场景使用）
 *
 * 删除守护任务并清空句柄，释放约 4KB 内部 RAM 栈。调用后本模块的
 * 定时/渐降/加时功能全部失效，仅适用于**马上要 esp_restart** 的路径
 * （如设置菜单"重新扫描"，见 ui/settings_menu.c 的 rescan_release_tasks）。
 * 幂等：任务已释放或从未创建时安全返回。
 */
void sleep_timer_release_task(void);

#ifdef __cplusplus
}
#endif
