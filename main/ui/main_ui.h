#pragma once

#include <stdbool.h>
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

/**
 * @brief 显示/隐藏整个主界面（lv_tabview 容器）
 *
 * 供全屏覆盖层（如设置菜单）使用：进入时隐藏主界面，可避免底层页面
 * （含播放页大封面图）参与重绘造成菜单滚动卡顿；退出时重新显示。
 * 仅切换 LV_OBJ_FLAG_HIDDEN，页面状态（当前 tab、滚动位置）全部保留。
 */
void main_ui_set_visible(bool visible);

#ifdef __cplusplus
}
#endif
