#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file list_common.h
 * @brief 列表页公共 UI 组件（页面样式 + 顶部标题栏）
 *
 * 供目录列表页（dir_list）与播放列表页（playlist_page）复用：
 *   - ui_page_style()：tab 页通用样式（黑底、无边框、无内边距、不可滚动）
 *   - ui_title_bar_create() / ui_title_bar_set_text()：顶部标题栏（高 44，
 *     左标题 + 右侧可选灰色计数文字，按 3:1 宽度划分互不覆盖，
 *     标题超长自动裁剪，底部细分隔线），文字可动态更新
 */

/** 标题栏高度（像素）：列表内容从它下方开始 */
#define UI_TITLE_BAR_H  44

/** 顶部状态栏高度（像素）：主界面容器撑满全屏（460x460），各页面内容
    从它下方开始（沉浸式布局预留）；状态栏本身、通知栏下拉触发区共用。
    改动只需改这里。 */
#define UI_STATUS_BAR_H  46

/**
 * @brief 应用 tab 页通用样式（黑底、无边框、无内边距、不可滚动）
 * @param page tab 页容器（lv_tabview_add_tab 的返回值）
 */
void ui_page_style(lv_obj_t *page);

/**
 * @brief 创建顶部标题栏（高 UI_TITLE_BAR_H，底部 1px 灰线分隔）
 * @param parent 父容器（tab 页）
 * @param title  标题文字（白色，占标题栏左侧 3/4 宽，超长自动裁剪）
 * @param sub    右侧计数/说明文字（灰色，占右侧 1/4 宽右对齐，可为 NULL）
 * @return 标题栏对象（后续用 ui_title_bar_set_text 更新文字）
 */
lv_obj_t *ui_title_bar_create(lv_obj_t *parent, const char *title,
                              const char *sub);

/**
 * @brief 更新标题栏文字
 * @param bar    ui_title_bar_create 返回的标题栏对象
 * @param title  新标题（NULL 表示不变）
 * @param sub    新计数文字（NULL 表示隐藏右侧文字；首次非 NULL 会自动创建）
 */
void ui_title_bar_set_text(lv_obj_t *bar, const char *title, const char *sub);

#ifdef __cplusplus
}
#endif
