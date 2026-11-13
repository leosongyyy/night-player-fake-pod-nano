#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电池百分比 UI 文本生成（状态栏 / 下拉控制中心共用）
 *
 * 读取 AXP2101 的电池快照（电压 + 电量计融合），生成"图标 + 百分比"文本：
 *   · 充电中 → 闪电图标；否则按电量档位用 BATTERY_FULL/3/2/1/EMPTY 图标；
 *   · 颜色：>20% 白、≤20% 黄、≤10% 红；PMU 不可用时灰色 "--%"。
 *
 * ⚠ 本函数会做 I²C 读取（约 1ms），只在 5 秒周期的 LVGL timer 里调用，
 *   不要在每帧刷新路径里调用。
 *
 * @param buf    输出缓冲（建议 ≥24 字节）
 * @param size   缓冲大小
 * @param color  输出文字颜色（可为 NULL）
 * @return true = 读到有效电量；false = PMU 不可用/读取失败（buf 已填 "--%"）
 */
bool battery_indicator_text(char *buf, size_t size, lv_color_t *color);

#ifdef __cplusplus
}
#endif
