#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键服务（S31：单键）
 *
 * ESP32-S31 / FakePodNano Pro V1 只有**一个独立 GPIO 按键**：
 *   - 下方独立按键：GPIO61，按键接 GND、低电平按下，内部上拉。
 *   - 上方电源键不再接主控 GPIO，由 AXP2101 读取（尚未适配，见
 *     core/utils/fake_shutdown.h 的占位说明）。
 *
 * 事件规则：
 * - 按下抬起：上报单击（若本次按压已上报过长按，则抬起不再报单击）；
 * - 按住超过 BUTTON_LONG_PRESS_MS（600ms）：上报长按（无需等待抬起）；
 * - 开机时已被按住（长按开机/唤醒后未及时松手）：首次抬起前不触发长按。
 *
 * 唤醒：S3 时代 B 键是 RTC GPIO / deepsleep 唤醒源；S31 的关机由 AXP2101
 * 负责，本键当前只做普通输入（不参与 deep sleep 唤醒）。
 */

/* 按键事件类型 */
typedef enum {
    BUTTON_EVENT_SHORT,   /* 单击（抬起时上报） */
    BUTTON_EVENT_LONG,    /* 长按（按住满阈值即上报，无需抬起） */
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
