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
 * 睡眠定时开启期间，用户动作（切歌/调音量等，UI 层在操作点调用
 * sleep_timer_note_activity）会触发加时：追加量 = (总时长 - 当前剩余) × 0.382
 * （0.382 = 1 - 0.618，黄金分割互补），即每次把剩余时间向总时长拉回 38.2%——
 * 剩余越少追加越多，连续触发逐次趋近总时长。原版无冷却：连续按键说明用户
 * 必然清醒，每次动作都立即追加。
 *
 * 注意：定时到点进入 deepsleep 后系统重启，本模块状态不保留；开机默认
 * 无定时（关闭）。开机是否自动开启由 config_store 的 sleep_def_on 控制。
 */

/**
 * @brief 初始化睡眠定时器（创建倒计时任务，可重复调用）
 */
void sleep_timer_init(void);

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
 * @brief 清醒感知：用户动作（切歌/调音量等）触发睡眠定时加时
 *
 * NightPlayer 原版算法：追加量 = (总时长 - 当前剩余时间) × 0.382
 * （0.382 = 1 - 0.618，黄金分割互补），把剩余时间向总时长拉回 38.2%，
 * 无冷却（连续操作说明用户必然清醒，每次都立即追加）。
 * 仅在定时开启且清醒感知开启（config_store 的 sleep_awake）时生效；
 * 由 UI 层在用户主动操作点调用（按键切歌/调音量、通知栏音量滑块、
 * 播放页控制、列表选歌等）。
 */
void sleep_timer_note_activity(void);

#ifdef __cplusplus
}
#endif
