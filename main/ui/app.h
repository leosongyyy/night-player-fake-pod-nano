#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用 UI 组装入口
 *
 * 依次创建主界面（main_ui）、顶部状态栏与下拉通知栏（notification_panel）。
 * 需在 lvgl_port_init() 之后、持有 LVGL 锁的情况下调用。
 */
void app_ui_create(void);

/**
 * @brief 更新状态栏中央的当前页面名称
 *
 * 由主界面（main_ui）在 tab 切换时调用；名称与四页 tab 下标对应
 * （0=目录/歌单、1=播放列表、2=正在播放、3=播放设置）。
 *
 * @param tab_index tab 下标（越界忽略）
 */
void app_status_set_page(uint8_t tab_index);

/**
 * @brief 设置状态栏可见性（正在播放页全屏封面时隐藏）
 *
 * 隐藏时背景变全透明、内容 label 隐藏，但**保留状态栏对象与触摸事件**
 * （通知栏下拉挂载在状态栏上，全屏封面时仍可下拉）。
 *
 * @param visible true 恢复半透明显示 / false 透明隐藏
 */
void app_status_set_ui_visible(bool visible);

#ifdef __cplusplus
}
#endif
