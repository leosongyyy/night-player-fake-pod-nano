#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键服务（A/B 双键）
 *
 * - A 键：GPIO48，GPIO-按钮-GND（外部无上拉，启用内部上拉）
 * - B 键：GPIO0，按键接 GND，外部已有 10kΩ 上拉（不启用内部上拉）
 *
 * 事件规则（均在上报前按"抬起"判定）：
 * - 单键按下抬起：报告 单击
 * - 双键同时按下（按下时间存在重叠）：全部抬起后上报一次双按事件
 */

/* 按键事件类型 */
typedef enum {
    BUTTON_EVENT_A_SHORT,   /* A 键（GPIO48）单击 */
    BUTTON_EVENT_B_SHORT,   /* B 键（GPIO0）单击 */
    BUTTON_EVENT_BOTH,      /* A/B 双键同时按下（全部抬起时上报） */
} button_event_t;

/* 按键事件回调（在按键任务上下文中调用，勿做耗时操作） */
typedef void (*button_event_cb_t)(button_event_t event, void *ctx);

/**
 * @brief 初始化按键服务：配置 GPIO 并启动轮询任务
 *
 * 事件默认打印到日志；也可通过 button_service_register_cb 订阅回调。
 */
void button_service_init(void);

/**
 * @brief 注册按键事件回调（仅支持一个回调，重复注册会覆盖）
 *
 * @param cb  回调函数，可为 NULL（只打印日志）
 * @param ctx 透传给回调的用户上下文
 */
void button_service_register_cb(button_event_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
