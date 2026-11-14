#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file clock_text.h
 * @brief 状态栏 / 下拉栏共用的时钟文本
 *
 * 读 PCF85063A RTC（本地墙钟时间），按 config_store 的 12/24 小时制设置格式化为
 * "HH:MM"：
 *   - 24 小时制（默认）：`00:00` ~ `23:59`；
 *   - 12 小时制：`01:00` ~ `12:59`，**不显示上下午**（用户要求）。
 *
 * RTC 不可用/时间非法时输出 `--:--`，界面不至于空白。
 *
 * ⚠ 会做一次 I²C 读取（约 1ms），只供 1 秒周期的时钟刷新使用，不要放进每帧绘制。
 */

/** @brief 格式化当前 RTC 时间为 "HH:MM"（含 12/24h 制式） */
void ui_clock_text(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
