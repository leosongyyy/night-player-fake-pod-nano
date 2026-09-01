#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file screen_sleep.h
 * @brief 息屏管理（息屏播放的核心开关）
 *
 * 息屏 = 屏幕全黑（亮度 0）+ 挂起 LVGL（停止刷屏、触摸轮询与 LVGL 时间）
 *        + CPU 固定降到 80MHz（esp_pm_configure 强制 max=min=80）。
 * 亮屏 = CPU 恢复 240MHz + 恢复 LVGL + 恢复用户配置亮度。
 *
 * ⚠ 不用 DFS 动态调频（实测释放频率锁后空闲期降频不明显），改为直接固定
 * 频率；若 80MHz 下 MP3 解码卡顿，可把 PM_CPU_FREQ_OFF_MHZ 改为 160。
 *
 * 入口：
 * - 手动切换：双键同时按下（BUTTON_EVENT_BOTH），见 ui/key_handler
 * - 自动息屏：无操作（按键/触摸）超过 config_store 的 screen_off_s 秒
 *   （默认 30 秒，设置页可调，NVS 持久化）后由本模块后台任务自动息屏。
 *
 * 息屏期间的行为（见 ui/key_handler）：
 * - A 键短按 = 播放/暂停；B 键短按 = 下一曲；B 键长按 = 上一曲；
 * - A 键长按 = 先亮屏再伪关机（等待断电，屏幕定格提示）；
 * - 双键同时按下 = 亮屏。
 *
 * 依赖：lvgl_port（挂起/恢复）、display（亮度）、esp_pm（固定频率）、
 *       config_store（超时配置）。需在 lvgl_port_init 之后调用 screen_sleep_init。
 */

/**
 * @brief 初始化息屏管理：固定 CPU 频率 240MHz、订阅触摸活动、启动
 *        无操作超时检测任务
 *
 * CONFIG_PM_ENABLE 未开启时降级：不调频，其余息屏功能（全黑 + 挂起 LVGL）
 * 仍正常。可重复调用（幂等）。
 */
void screen_sleep_init(void);

/** @brief 进入息屏（全黑 + 挂起 LVGL + CPU 锁 80MHz）。已在息屏时直接返回。
 *         线程安全：内部持状态互斥锁，可从按键任务/检测任务调用。 */
void screen_sleep_enter(void);

/** @brief 退出息屏（恢复亮度 + 恢复 LVGL + CPU 恢复 240MHz）。未息屏时直接返回。
 *         线程安全。退出后重置"最近活动时间"，避免立刻再次自动息屏。 */
void screen_sleep_exit(void);

/** @brief 息屏/亮屏切换（双键同时按下调用） */
void screen_sleep_toggle(void);

/** @brief 当前是否处于息屏状态 */
bool screen_sleep_is_sleeping(void);

/**
 * @brief 记录一次用户活动（按键/触摸），刷新自动息屏的计时基准
 *
 * 任意线程可调用（内部只写一个 int64 时间戳）。息屏状态下调用也会刷新
 * 计时，但息屏时检测任务跳过检查，无副作用；退出息屏时会自动重置。
 */
void screen_sleep_note_activity(void);

/**
 * @brief 距最近一次用户活动的空闲秒数（无操作已持续多久）
 *
 * 供 UI 复用"空闲计时"实现挂机行为（如正在播放页挂机 10 秒自动隐藏
 * 标题栏/控制栏）。任意线程可调用。
 */
uint32_t screen_sleep_get_idle_sec(void);

#ifdef __cplusplus
}
#endif
