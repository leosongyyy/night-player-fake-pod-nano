#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file settings_page.h
 * @brief 播放设置页（四页滑动主界面的第 4 页）
 *
 * 当前为占位界面，仅展示标题与开发提示。
 */

/**
 * @brief 创建播放设置页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *settings_page_create(lv_obj_t *tv);

#ifdef __cplusplus
}
#endif
