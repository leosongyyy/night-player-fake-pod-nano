#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file playlist_page.h
 * @brief 播放列表页（四页滑动主界面的第 2 页）
 *
 * 用通用虚拟列表组件（core/ui/virtual_list）展示**某个目录/歌单**的歌曲
 * （8 字节 playlist_song_entry_t 条目），支持虚拟滚动、原生滚动条、
 * 点击选中；当前播放歌曲文字黄色高亮。
 *
 * 显示内容**不自动跟随播放状态**，仅在以下两个入口更新：
 *   - playlist_page_show_dir()：目录列表页点击目录后调用（随后自动切到本页）；
 *   - playlist_page_show_current()：正在播放页"转到列表"按钮调用。
 * 开机默认在播放器就绪后显示正在播放的目录（playlist_page_show_current）。
 *
 * 点击歌曲 → player_controller_play_song() 直接播放。
 *
 * 需在持有 LVGL 锁（lvgl_port_lock）时调用。
 */

/**
 * @brief 创建播放列表页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *playlist_page_create(lv_obj_t *tv);

/**
 * @brief 把播放列表更新为指定目录/歌单（index 越界忽略）
 * @param dir_index 目录下标（0 .. playlist_dir_count()-1）
 */
void playlist_page_show_dir(uint32_t dir_index);

/**
 * @brief 把播放列表更新为正在播放的目录（无播放时显示空提示）
 *
 * 供"正在播放"页的"查看列表"按钮调用。
 */
void playlist_page_show_current(void);

/**
 * @brief 开关播放列表的超长文本跑马灯（页面不可见时关闭省电）
 * @param enable true 开启 / false 关闭（裁剪）
 */
void playlist_page_set_marquee(bool enable);

#ifdef __cplusplus
}
#endif
