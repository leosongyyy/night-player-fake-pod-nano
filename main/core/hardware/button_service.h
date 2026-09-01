#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键服务（A/B 双键）
 *
 * - A 键：GPIO48，GPIO-按钮-GND（外部无上拉，启用内部上拉）。
 *   ⚠ A 键与成品机电源侧的 3 秒关机芯片复用同一按键：长按超过 3 秒
 *   由硬件直接断电/开机。固件在长按 2 秒时主动伪关机（见 fake_shutdown）。
 * - B 键：GPIO0，按键接 GND，外部已有 10kΩ 上拉（不启用内部上拉）。
 *   B 键是 RTC GPIO，可作 deepsleep 唤醒源（伪关机后按下可取消关机并唤醒）。
 *
 * 事件规则：
 * - 单键按下抬起：报告 单击（若该次按压已上报过长按，则抬起不再报单击）
 * - 单键按住超过各自阈值：上报 长按（无需等待抬起）
 *   - A 键（GPIO48）：BUTTON_A_LONG_PRESS_MS，默认 1200ms（与电源侧
 *     3 秒关机芯片配合，见 Kconfig CONFIG_BUTTON_LONG_PRESS_MS）
 *   - B 键（GPIO0）：BUTTON_B_LONG_PRESS_MS，固定 600ms（可安全长按，
 *     亮屏=音量-10 / 息屏=上一曲，缩短阈值提升响应）
 * - 双键同时按下（按下时间存在重叠）：全部抬起后上报一次双按事件；
 *   双按交互期间不触发长按
 * - 开机时已被按住的键（如长按 3s 开机后未及时松手）：首次抬起前不触发
 *   长按，抬起后恢复正常检测
 */

/* 按键事件类型 */
typedef enum {
    BUTTON_EVENT_A_SHORT,   /* A 键（GPIO48）单击 */
    BUTTON_EVENT_B_SHORT,   /* B 键（GPIO0）单击 */
    BUTTON_EVENT_A_LONG,    /* A 键（GPIO48）长按：电源键，按住满阈值即上报 */
    BUTTON_EVENT_B_LONG,    /* B 键（GPIO0）长按 */
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
