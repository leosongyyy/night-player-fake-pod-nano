#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file key_handler.h
 * @brief 应用层按键分发（S31 单键版）
 *
 * ESP32-S31 / FakePodNano Pro V1 只有一个独立 GPIO 按键（GPIO61，下方按键）；
 * 上方电源键由 AXP2101 读取（尚未适配）。
 *
 * 亮屏状态：
 *   - 短按：音量 -1 档（音量 0~30 档）
 *   - 长按（600ms）：音量一次性 -10 档
 * 息屏状态（黑屏播放，按键盲操作）：
 *   - 短按：亮屏 + 下一曲
 *   - 长按：亮屏 + 上一曲
 *
 * 说明：
 * - 音量 + 目前只能在下拉控制中心/设置里调（单键无法同时承担 +/-）；
 * - S3 的 A 键（电源/音量+）、双键组合（息屏切换）与长按伪关机都因硬件
 *   变更移除，真正的关机由 AXP2101 负责（待适配）。
 *
 * 所有按键事件都会记录为一次"用户活动"（刷新自动息屏计时，
 * 见 core/utils/power_service）。
 *
 * 唤醒防循环：当前 S31 不使用 deep sleep 伪关机，但仍保留开机 1.2 秒内
 * 忽略按键事件的保护（防止 AXP2101 开机/唤醒瞬间的按键残留触发误动作）。
 */

/**
 * @brief 初始化按键分发：注册 button_service 按键事件回调
 *
 * 需在 button_service_init() 之后调用（内部调用 button_service_register_cb）。
 */
void key_handler_init(void);

#ifdef __cplusplus
}
#endif
