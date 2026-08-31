#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file now_playing.h
 * @brief 正在播放页（四页滑动主界面的第 3 页）
 *
 * 布局（全屏 460x460）：
 *   - 封面图：460x460 全屏（目前用默认封面 default_cover，后续接入读取）；
 *     点击封面切换标题栏/控制栏的显示与隐藏；
 *   - 顶部标题栏（50% 半透明黑，状态栏下方）：大字体歌曲标题 + 小字体
 *     专辑/艺术家（暂用播放列表名代替），超长文本跑马灯（非当前页关闭）；
 *     点击标题栏 → 播放列表更新为正在播放的目录并跳转到播放列表页；
 *   - 底部控制栏（50% 半透明黑）：当前时长 + 上一曲/播放暂停/下一曲 +
 *     总时长 + 进度条（拖动松手 seek）。
 *
 * 数据均来自 player_controller_get_status()，由内部 timer 周期刷新。
 */

/**
 * @brief 创建正在播放页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *now_playing_create(lv_obj_t *tv);

/**
 * @brief 开关标题/副标题的跑马灯（页面不在可视区时关闭省电）
 * @param enable true 开启 / false 关闭（裁剪）
 */
void now_playing_set_marquee(bool enable);

/**
 * @brief 强制恢复标题栏/控制栏显示
 *
 * 由 main_ui 在离开正在播放页时调用：用户在播放页点封面隐藏的
 * 栏（以及联动的状态栏）在下次进入前复位显示。
 */
void now_playing_force_show_ui(void);

#ifdef __cplusplus
}
#endif
