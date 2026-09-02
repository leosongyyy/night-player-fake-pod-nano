#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file key_handler.h
 * @brief 应用层按键分发（亮屏/息屏两套按键映射）
 *
 * 亮屏状态：
 *   - A 键（GPIO48，上键）短按：音量 +
 *   - B 键（GPIO0，下键）短按：音量 -
 *   - A 键长按：伪关机（等待断电，屏幕定格提示，配合 3 秒关机芯片）
 *   - B 键长按：音量一次性 -10 档（B 键可安全长按；音量 0~30 档）
 *   - 双键同时按下：息屏
 * 息屏状态（黑屏播放，按键盲操作）：
 *   - A 键短按：播放 / 暂停
 *   - B 键短按：下一曲
 *   - B 键长按：上一曲（B 键可安全长按）
 *   - A 键长按：先亮屏再伪关机（点亮屏幕显示保存/关机提示，计划第 6 点）
 *   - 双键同时按下：亮屏
 *
 * 所有按键事件都会记录为一次"用户活动"（刷新自动息屏计时，
 * 见 core/utils/power_service）。
 *
 * 唤醒防循环：deepsleep 唤醒 = 芯片重启，若唤醒时按键仍被按住，开机后
 * 去抖仍会报告事件 → 立即再次休眠；故开机 1.2 秒内忽略按键事件。
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
