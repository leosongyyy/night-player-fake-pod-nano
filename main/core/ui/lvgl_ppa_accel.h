/**
 * @file lvgl_ppa_accel.h
 * LVGL 9.4 自定义 SW blend handler —— 用 ESP32-S31 的 PPA 做硬件混合
 *
 * 设计参考 OtoClock / esp_lvgl_adapter 的 lvgl_ppa_accel_v9.c：
 * 不启用 LVGL 自带的 PPA draw unit（它对局部刷新支持不完整），而是用
 * lv_draw_sw_register_blend_handler() 注册一个 RGB565 目标的混合回调，
 * 在回调里按"面积 / 格式 / 对齐"决定走 PPA 还是回退 CPU。
 *
 * 由 CONFIG_UI_PPA_ACCEL 控制编译；关闭时所有函数退化为空实现。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 PPA 混合加速（必须在 lv_init() 与显示对象创建之后调用一次）。
 *
 * @param disp  目标显示对象（只有 RGB565 显示会被接管）
 */
void lvgl_ppa_accel_init(lv_display_t *disp);

/**
 * PPA / CPU 混合性能自测（调试用，CONFIG_UI_PPA_BENCH 开启时有效）。
 * 在启动阶段打印同一块 460x46 缓冲上 CPU 与 PPA 的耗时对比，
 * 用于决定 CONFIG_UI_PPA_BLEND_MIN_PX 的取值。
 */
void lvgl_ppa_accel_benchmark(void);

#ifdef __cplusplus
}
#endif
