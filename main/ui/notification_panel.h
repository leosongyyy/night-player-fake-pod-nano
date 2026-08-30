#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 下拉通知栏（全屏面板）
 *
 * 包含：电池电压显示、亮度滑块、底部上拉把手，以及
 * 顶部下拉/底部上拉的手势拖拽与展开/收起动画。
 * 拖拽由 LVGL 触摸事件驱动（复用 indev 触摸数据，无独立轮询任务）。
 *
 * @param status_bar 顶部状态栏句柄（面板收起时在其上接收下拉手势）
 */
void notification_panel_create(lv_obj_t *status_bar);

#ifdef __cplusplus
}
#endif
