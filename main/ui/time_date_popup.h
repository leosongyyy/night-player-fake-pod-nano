#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hardware/rtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file time_date_popup.h
 * @brief 滚轮弹窗集合：日期 / 时间 / 时区 / 小时区间
 *
 * 四个弹窗都挂在 lv_layer_top() 上（全屏遮罩 + 中央面板），覆盖设置菜单与
 * 状态栏；点击面板外 = 取消。面板内是 LVGL roller 滚轮：
 *   - 设置日期：年 / 月 / 日三列，日数随年月（含闰年）动态变化；
 *   - 设置时间：时 / 分 / 秒三列；
 *   - 时区设置：单个滚轮，UTC-12:00 ~ UTC+14:00 的常用偏移（含半小时、
 *     三刻钟档）；
 *   - 小时区间：开始 / 结束两个滚轮（00:00~23:00），供睡眠定时器的
 *     "按时间段"使用，两者不可相同。
 *
 * RTC 里存的是**本地墙钟时间**（PCF85063A 没有时区概念）：
 *   - 手动设置日期/时间直接按用户看到的本地时间写入；
 *   - 修改时区时把 RTC 平移 (new - old) 分钟，保持绝对时刻不变；
 *   - 网络对时按"UTC + 时区偏移"写入（见 core/network/wifi_time_sync.c）。
 *
 * ⚠ 所有函数都必须在 LVGL 任务/持锁上下文调用。
 */

/** @brief 外部状态刷新回调（弹窗保存成功 / 网络对时写 RTC 后调用，用于刷新页面右侧状态） */
void time_date_popup_set_refresh_cb(void (*cb)(void));

/** @brief 触发一次外部状态刷新（无回调时无操作） */
void time_date_popup_notify_changed(void);

/** @brief 打开「设置日期」滚轮弹窗（初始值取当前 RTC 时间） */
void time_date_popup_show_date(void);

/** @brief 打开「设置时间」滚轮弹窗（初始值取当前 RTC 时间） */
void time_date_popup_show_time(void);

/** @brief 打开「时区设置」滚轮弹窗（初始值取 config_store 时区） */
void time_date_popup_show_tz(void);

/**
 * @brief 小时区间选择回调（点了"保存"且校验通过后调用）
 * @param start_hour 开始小时 0~23
 * @param end_hour   结束小时 0~23（保证与 start_hour 不同）
 */
typedef void (*time_date_hour_range_cb_t)(uint8_t start_hour, uint8_t end_hour);

/**
 * @brief 打开"小时区间"滚轮弹窗（开始 / 结束两个滚轮，只选整点）
 *
 * 供「睡眠定时器 → 时间段」使用：两个滚轮都是 00:00 ~ 23:00，用户点保存时
 * 校验**两者不可相同**（相同则弹 Toast 并保持弹窗，不保存）。区间语义为
 * 左闭右开、允许跨零点（22 → 6 表示 22:00~05:59），由调用方解释。
 *
 * @param title      面板标题
 * @param start_hour 初始开始小时（0~23，越界收敛到 0）
 * @param end_hour   初始结束小时（0~23，越界收敛到 0）
 * @param on_save    保存回调（可为 NULL）
 */
void time_date_popup_show_hour_range(const char *title, uint8_t start_hour,
                                     uint8_t end_hour,
                                     time_date_hour_range_cb_t on_save);

/** @brief 是否有任一弹窗正在显示（按键/手势可据此忽略） */
bool time_date_popup_any_visible(void);

/* ------------------------------------------------------------------ */
/* 格式化辅助（设置菜单复用，避免两处格式漂移）                          */
/* ------------------------------------------------------------------ */

/** @brief "YYYY-MM-DD" */
void time_date_format_date(const rtc_time_t *t, char *buf, size_t size);

/** @brief "HH:MM:SS" */
void time_date_format_time(const rtc_time_t *t, char *buf, size_t size);

/** @brief "UTC+8" / "UTC-3:30" / "UTC+5:45" */
void time_date_format_tz(int minutes, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
