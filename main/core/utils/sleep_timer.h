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
 * 设置时长（分钟）后，独立任务（Core1）每 10 秒检查一次绝对到期时间，
 * 倒计时结束调用 fake_shutdown_enter()（保存播放位置、优雅停播、关外设、
 * 进入深度休眠）。
 *
 * 注意：定时到点进入 deepsleep 后系统重启，本模块状态不保留；开机默认
 * 无定时（关闭）。
 */

/**
 * @brief 初始化睡眠定时器（创建倒计时任务，可重复调用）
 */
void sleep_timer_init(void);

/**
 * @brief 设置/取消睡眠定时
 * @param minutes 分钟数；0 = 取消定时；>0 = 从现在起倒计时 minutes 分钟
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

#ifdef __cplusplus
}
#endif
