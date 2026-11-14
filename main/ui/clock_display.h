#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file clock_display.h
 * @brief 顶部时钟文本的集中显示管理（状态栏 / 下拉控制中心顶部共用）
 *
 * 状态栏左侧和下拉通知栏顶部左侧显示同一份时间文本，另外还需要支持
 * **临时状态**——例如调音量时把时间临时换成音量数值、约 3 秒后自动回到时间。
 * 这两件事收在本模块里：
 *
 *   - 挂载：创建好 label 后调用 clock_display_attach(label)（可挂多个，
 *     模块把同一份文本写给所有已挂 label；label 被删除时自动摘除）；
 *   - 刷新：模块自带 1 秒 lv_timer，时间文本由它统一刷新（12/24 小时制
 *     来自 config_store），调用方不必再各建一个 1 秒定时器；
 *   - 临时状态：clock_display_show_temp(text) 显示约 3 秒后自动回到时间，
 *     或用 clock_display_show_temp_for(text, sec) 指定秒数。任何"想在时钟
 *     位置短提示一下"的功能（音量、静音、对时结果……）都走这里。
 *
 * ⚠ 所有接口必须在 LVGL 任务上下文（持有 LVGL 锁）调用。
 * ⚠ 定时器在第一个 label 挂上时创建，之后常驻。
 * ⚠ 临时状态的时长按"秒计数器"计：每次调用把计数器置为 N，1 秒定时器
 *   逐拍递减，归零那一拍写回时间——所以实际保持 N-1~N 秒，不追求严格
 *   的毫秒级精度（用户明确表示不需要严格 3 秒）。
 */

/** @brief 挂载一个用于显示时间/临时状态的 label（同一 label 重复挂载会被忽略） */
void clock_display_attach(lv_obj_t *label);

/** @brief 显示临时状态文本，约 3 秒后自动回到时间 */
void clock_display_show_temp(const char *text);

/** @brief 显示临时状态文本，指定持续秒数（1~255，每次调用都重置计时） */
void clock_display_show_temp_for(const char *text, uint8_t seconds);

/** @brief 立即按当前时制重刷一次时间（临时状态显示期间调用会被忽略） */
void clock_display_refresh(void);

#ifdef __cplusplus
}
#endif
