#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file button_service.h
 * @brief 按键服务（S31 双键：上=PMU 电源键，下=GPIO61）
 *
 * FakePodNano Pro V1 的两个键来源不同：
 *   - **上键（电源键）**：接 AXP2101 的 PWRON，主控没有 GPIO；由 PMU 的
 *     `0x49` 按下/抬起边沿事件维护"当前是否按住"（`axp2101_is_power_key_pressed()`）。
 *   - **下键**：GPIO61，低电平按下、内部上拉，直接轮询电平。
 *
 * 本模块把两个来源统一成 S3 时代的按键事件（含**双键同时按下**判定）：
 *   - 上键短按/长按、下键短按/长按、双键同时按下；
 *   - 长按判定要求"另一键未按下"（双按交互不触发长按）；
 *   - 双按以"两键按下时间存在重叠"为准，全部抬起后统一上报一次 BOTH，
 *     并抑制本次交互的单键事件。
 *
 * 长按阈值：
 *   - 上键：默认 1200ms，可由 `button_service_set_up_long_press_provider()`
 *     在按下时动态取值（本项目亮屏 1200ms / 息屏 900ms，与 S3 一致）；
 *   - 下键：600ms（固定）。
 *
 * ⚠ 上键的"按住"状态依赖 PMU 中断任务更新，延迟约 5~10ms；人手的按键
 *   持续时间为几十 ms 以上，足够判定。PMU 自己的短按/长按 IRQ 只做状态清理，
 *   业务语义统一由本模块生成（避免两套阈值/重复事件）。
 */

/* 按键事件类型（上键=PMU 电源键，下键=GPIO61） */
typedef enum {
    BUTTON_EVENT_UP_SHORT,     /* 上键短按 */
    BUTTON_EVENT_UP_LONG,      /* 上键长按（默认 1200ms，息屏 900ms） */
    BUTTON_EVENT_DOWN_SHORT,   /* 下键短按（抬起时上报） */
    BUTTON_EVENT_DOWN_LONG,    /* 下键长按（600ms，不需抬起） */
    BUTTON_EVENT_BOTH,         /* 双键同时按下（全部抬起后上报一次） */
} button_event_t;

/* 按键事件回调（在按键任务上下文中调用，勿做耗时操作） */
typedef void (*button_event_cb_t)(button_event_t event, void *ctx);

/* 上键长按阈值动态提供者：每次按下时调用一次，返回本次阈值（ms）；
   返回 0 表示沿用默认值。 */
typedef uint32_t (*button_long_press_provider_t)(void);

/**
 * @brief 初始化按键服务：配置 GPIO61、启动 5ms 轮询任务
 *
 * 上键状态由 AXP2101 驱动提供（需先完成 axp2101_init，见 battery_init）。
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

/**
 * @brief 设置上键长按阈值的动态提供者（可选）
 *
 * 在每次上键按下沿调用一次，返回本次按压使用的阈值（ms），0 = 用默认值。
 * 本项目用它实现"亮屏 1200ms / 息屏 900ms"（见 ui/key_handler.c）。
 */
void button_service_set_up_long_press_provider(button_long_press_provider_t fn);

#ifdef __cplusplus
}
#endif
