#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用 UI 组装入口
 *
 * 依次创建主界面（main_ui）与下拉通知栏（notification_panel）。
 * 需在 lvgl_port_init() 之后、持有 LVGL 锁的情况下调用。
 */
void app_ui_create(void);

#ifdef __cplusplus
}
#endif
