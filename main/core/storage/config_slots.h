#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config_slots.h
 * @brief 配置槽位（SAVE1~SAVE4）的**运行时应用**（NightPlayer「配置插槽」）
 *
 * config_store 负责"存哪一份"（内存 + NVS，应用侧 get_xxx / set_xxx 无感），
 * 本模块负责"换一份之后让全系统跟着变"——即把一个槽位的内容真正落到硬件与
 * 播放器上：
 *   - 音量     → audio_set_volume_level
 *   - EQ 预设  → eq_apply
 *   - 循环 / 随机 / 当前列表与歌曲 → player_controller_restore_config
 *     （控制器在 Core0 任务上下文原子执行，避免与切歌命令抢时序）
 *
 * ⚠ **睡眠定时不在应用范围内**（2026-11 起与配置槽位互相独立）：它的时长与
 *   "开机默认开启"开关都是全局配置，切换槽位既不改变它们、也不重新开表，
 *   正在跑的定时继续跑、没开的定时不会被切配置打开。
 *
 * ⚠ 调用时机：必须在 **LVGL 上下文（持锁）**中调用（UI 事件回调内即可）——
 *   内部会调 config_store_switch_slot()，它同步触发 config_store 变更回调，
 *   回调方（设置页）直接操作 LVGL 控件。config_slots_switch 首次触屏时还会
 *   懒加载槽位弹窗。
 *
 * ⚠ 切换前会先把"当前这套配置"存回原槽位（config_store_switch_slot 内部），
 *   所以"切走再切回"回到离开时的列表/歌曲/音量/EQ。
 */

/**
 * @brief 切换配置槽位并立即应用（UI 点击槽位列表时调用）
 *
 * 流程：
 *   1. 当前活跃槽位 ≠ idx 时：先把当前配置存回原槽位（存储层内部做）；
 *   2. 载入 idx 槽位（空槽位 = 出厂默认 + 无位置记录）；
 *   3. 应用音量 / EQ（同步，LVGL 上下文）；
 *   4. 让播放控制器恢复循环、随机与当前歌曲并**自动播放**；
 *   5. Toast 提示"已切换到 SAVE<n>"。
 *
 * 睡眠定时与槽位无关：本函数**不碰**睡眠定时器（见上方说明）。
 *
 * @param idx 目标槽位下标（0 起）；越界或与当前槽位相同则不做任何事
 */
void config_slots_switch(uint32_t idx);

#ifdef __cplusplus
}
#endif
