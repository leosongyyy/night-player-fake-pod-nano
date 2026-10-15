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
 * @brief 供 LCD 面板 IO 使用的 flush 完成回调（内部调用 lv_display_flush_ready）
 */
bool lvgl_port_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);

/**
 * @brief 获取"保存 LVGL 显示对象指针的变量地址"（供 LCD 面板 IO 作为回调上下文）
 *
 * ⚠ LVGL 9 的显示对象要等 lvgl_port_init() 才由 lv_display_create() 创建，
 * 而 display_init() 早于它、且必须先把回调上下文交给 esp_lcd；因此这里返回的
 * 是内部 `lv_display_t *` 变量的地址（稳定），flush 完成回调触发时解引用即可
 * 拿到当时的显示对象（未创建时为 NULL，回调内部判空）。
 *
 * @return 指向内部显示对象指针的地址（生命周期为整个进程）
 */
void *lvgl_port_get_flush_ctx(void);

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
 * 支持多个订阅者（内部按固定容量数组保存，见 lvgl_port.c 的 TOUCH_CB_MAX）；
 * 相同 cb+ctx 重复注册会被忽略。回调运行在 LVGL 任务上下文（已持锁），
 * 可安全调用 LVGL API；也可只做轻量状态记录（如息屏活动的"最近操作时间"）。
 *
 * @param cb       回调函数（传 NULL 忽略）
 * @param user_ctx 透传给回调的用户上下文
 */
void lvgl_port_register_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx);

/**
 * @brief 注销触摸事件回调（按 cb + user_ctx 精确匹配）
 *
 * 仅注销注册时 cb 与 user_ctx 都相同的那个订阅；同一回调为多个上下文
 * 注册过时（如多个 ClickWheel 实例），只移除匹配的那一个。
 *
 * @param cb       之前注册过的回调函数；传 NULL 忽略
 * @param user_ctx 注册时传入的用户上下文（需与注册时一致）
 */
void lvgl_port_unregister_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx);

/**
 * @brief 设置触摸句柄（触摸异步初始化完成后调用）
 *
 * 触摸控制器初始化在后台任务中异步执行（touch_init() 立即返回），
 * 就绪后由上层调用本接口把句柄交给 LVGL 输入设备。
 * 在句柄就绪前，触摸 read_cb 返回"未按下"（不给数据），就绪后自动恢复。
 *
 * @param touch_handle 就绪的触摸句柄
 */
void lvgl_port_set_touch_handle(esp_lcd_touch_handle_t touch_handle);

/**
 * @brief 挂起 LVGL（停止 tick 计时 + 挂起 LVGL 任务）
 *
 * 供息屏/深度休眠前调用：停止 lv_tick（LVGL 时间冻结，恢复后动画从暂停点
 * 继续）并挂起 LVGL 任务（停止刷屏与触摸轮询）。恢复用 lvgl_port_resume。
 * 深度休眠（deepsleep）场景无需恢复，唤醒后系统重启重新初始化。
 */
void lvgl_port_suspend(void);

/**
 * @brief 恢复 LVGL（恢复任务 + 重新启动 tick 计时）
 *
 * 与 lvgl_port_suspend 配对，供息屏唤醒后调用。恢复后 LVGL 任务继续
 * 刷屏与触摸轮询，tick 时间从挂起时刻继续（无跳变）。
 */
void lvgl_port_resume(void);

#ifdef __cplusplus
}
#endif
