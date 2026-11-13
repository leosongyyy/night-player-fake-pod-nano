#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file key_handler.h
 * @brief 应用层按键分发（S31 双键，恢复 S3 时代的映射）
 *
 * 两个键来源不同，但统一由 core/hardware/button_service 生成事件：
 *   - **上键**：AXP2101 的 PWRON（电源键，无主控 GPIO）；
 *   - **下键**：GPIO61（下方独立按键）。
 *
 * 亮屏状态：
 *   - 上键短按：音量 +1 档
 *   - 下键短按：音量 -1 档
 *   - 下键长按（600ms）：音量一次性 -10 档（暂定）
 *   - 上键长按（1200ms）：关机
 *   - 双键同时按下：息屏
 *
 * 息屏状态（黑屏播放，按键盲操作）：
 *   - 上键短按：播放/暂停
 *   - 下键短按：下一曲
 *   - 下键长按（600ms）：上一曲
 *   - 上键长按（900ms）：先亮屏显示「正在关机...」，再保存数据并 PMU 断电
 *   - 双键同时按下：亮屏
 *
 * 说明：
 * - 长按上键由固件在 1.2s/0.9s 触发软关机；继续按住到 PMU 的 OFFLEVEL
 *   （实测 4s）会由 PMU 硬件强制断电，作为兜底。
 * - 双键判定以"两键按下时间存在重叠"为准，全部抬起后统一上报一次，
 *   并抑制本次交互的单键事件（见 button_service）。
 *
 * 所有按键事件都会记录为一次"用户活动"（刷新自动息屏计时，
 * 见 core/utils/power_service）。
 *
 * 唤醒防循环：保留开机 1.2 秒内忽略按键事件的保护（防止 AXP2101 开机
 * 瞬间的按键残留触发误动作）。
 */

/**
 * @brief 初始化按键分发：注册 button_service 事件回调与上键长按阈值提供者
 *
 * 需在 button_service_init() 之后调用（内部调用 button_service_register_cb）。
 */
void key_handler_init(void);

#ifdef __cplusplus
}
#endif
