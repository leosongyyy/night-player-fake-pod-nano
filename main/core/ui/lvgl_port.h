#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LVGL 移植层（显示驱动注册 + tick + 任务 + 互斥锁 + 触摸输入设备）
 *
 * 依赖 display_init() 返回的面板句柄；向 LVGL 注册 flush/rounder 回调，
 * 并启动专用 LVGL 任务（lv_timer_handler 循环）。
 */

/**
 * @brief 初始化 LVGL 并注册显示驱动与触摸输入设备
 *
 * @param panel_handle display_init() 返回的面板句柄
 * @param touch_handle touch_init() 返回的触摸句柄（可为 NULL，则不注册输入设备）
 */
void lvgl_port_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch_handle);

/**
 * @brief 获取 LVGL 互斥锁（LVGL API 非线程安全，UI 操作前需加锁）
 *
 * @param timeout_ms 超时时间；-1 表示永久等待
 * @return true 获取成功
 */
bool lvgl_port_lock(int timeout_ms);

/**
 * @brief 释放 LVGL 互斥锁
 */
void lvgl_port_unlock(void);

/**
 * @brief 供 LCD 面板 IO 使用的 flush 完成回调（内部调用 lv_disp_flush_ready）
 */
bool lvgl_port_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);

/**
 * @brief 获取 LVGL 显示驱动实例指针（供 LCD 面板 IO 作为回调上下文使用）
 */
lv_disp_drv_t *lvgl_port_get_disp_drv(void);

/**
 * @brief 触摸事件回调类型
 *
 * 由 LVGL 输入设备的触摸轮询（indev read_cb）驱动，调用频率与
 * LVGL indev 轮询周期一致。回调运行在 LVGL 任务上下文中（已持锁），
 * 可安全调用 LVGL API。
 *
 * @param pressed  是否按下
 * @param x        触摸 x 坐标
 * @param y        触摸 y 坐标
 * @param user_ctx 注册时传入的用户上下文
 */
typedef void (*lvgl_port_touch_cb_t)(bool pressed, uint16_t x, uint16_t y, void *user_ctx);

/**
 * @brief 注册触摸事件回调（复用 LVGL 输入设备的触摸轮询，无需额外定时器）
 *
 * @param cb       回调函数；传 NULL 取消注册
 * @param user_ctx 透传给回调的用户上下文
 */
void lvgl_port_register_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx);

/**
 * @brief 挂起 LVGL 任务（停止刷屏与触摸轮询），供深度休眠前调用
 *
 * 唤醒后系统重启会重新初始化，无需恢复接口。
 */
void lvgl_port_suspend(void);

#ifdef __cplusplus
}
#endif
