#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dir_list.h
 * @brief 目录/歌单列表页（四页滑动主界面的第 1 页）
 *
 * 用通用虚拟列表组件（core/ui/virtual_list）展示全部目录/歌单
 * （12 字节 playlist_dir_entry_t 条目），整页为一张深灰卡片（上圆下直、
 * 左右 10px 黑边）：顶部标题栏固定显示 SD 卡图标 + /sdcard + 条目数，
 * 行左侧图标按名字区分（.pl 结尾 → LV_SYMBOL_LIST，否则 LV_SYMBOL_DIRECTORY）；
 * 支持虚拟滚动、原生滚动条、点击选中；
 * 当前播放目录文字黄色高亮。
 *
 * 点击某个目录 → 播放列表页更新为该目录，并自动切换到播放列表页
 * （播放列表页不会自动跟随，仅此入口与"转到列表"按钮更新）。
 *
 * 需在持有 LVGL 锁（lvgl_port_lock）时调用。
 */

/**
 * @brief 创建目录列表页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *dir_list_create(lv_obj_t *tv);

/**
 * @brief 开关目录列表的超长文本跑马灯（页面不可见时关闭省电）
 * @param enable true 开启 / false 关闭（裁剪）
 */
void dir_list_set_marquee(bool enable);

#ifdef __cplusplus
}
#endif
