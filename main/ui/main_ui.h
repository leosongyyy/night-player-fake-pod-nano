#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主界面（四页横向滑动，类 Android ViewPager）
 *
 * 基于 LVGL 内置 lv_tabview（demo_widgets 同款），隐藏顶部 tab 按钮条后
 * 即为纯左右滑动翻页。四个页面：
 *   - 0 目录列表（ui/dir_list）：全部目录/歌单虚拟列表，点击目录 →
 *     播放列表页更新为该目录并切换到播放列表页；
 *   - 1 播放列表（ui/playlist_page）：某个目录的歌曲虚拟列表，点击播放；
 *     内容不自动跟随播放，仅"点击目录"或"转到列表"按钮更新；
 *   - 2 正在播放（ui/now_playing）：占位 + "转到列表"按钮；
 *   - 3 播放设置（ui/settings_page）：占位。
 *
 * 页面切换（VALUE_CHANGED）时控制两个列表的跑马灯：非列表页关闭省电，
 * 并同步状态栏中央的当前页面名称。
 *
 * 需在持有 LVGL 锁（lvgl_port_lock）时调用。
 */
void main_ui_create(void);

/**
 * @brief 切换到指定页面（带动画，等同 ViewPager 翻页）
 * @param tab tab 下标（0~3，越界自动收敛）
 */
void main_ui_switch_tab(uint8_t tab);

/**
 * @brief 获取当前页面下标
 * @return 当前 tab 下标（0~3；主界面未创建时返回 0）
 */
uint8_t main_ui_get_current_tab(void);

#ifdef __cplusplus
}
#endif
