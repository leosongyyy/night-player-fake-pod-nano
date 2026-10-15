#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JPEG 封面解码（esp_jpeg / tjpgd 软件解码 → LVGL 图片）
 *
 * 上位机 .npd 里存放的是 460x460 的 JPEG（黑底居中），本模块将其解码为
 * RGB565 像素缓冲并封装成 lv_image_dsc_t，可直接 lv_image_set_src 显示。
 *
 * ⚠ 解码较慢（460x460 约 100~300ms），必须放在后台任务中调用，
 *   完成后持 LVGL 锁再更新 UI，禁止在 LVGL 任务/回调中直接调用。
 *
 * ⚠ RGB565 字节序（LVGL 9 迁移后）：解码输出为**本机小端** RGB565，
 *   即 LVGL 9 渲染缓冲的原生字节序。v8 时代这里输出"高字节在前"以匹配
 *   CONFIG_LV_COLOR_16_SWAP=y；v9 移除了该配置项，改由移植层在 flush 回调里
 *   对整块缓冲调用 lv_draw_sw_rgb565_swap()（见 core/ui/lvgl_port.c）。
 */

/**
 * @brief 解码 JPEG 数据为 LVGL RGB565 图片
 *
 * @param jpeg JPEG 数据（可位于 PSRAM）
 * @param len  JPEG 数据长度
 * @param out  输出图片描述符（成功时 data 指向新分配的像素缓冲，
 *             用完必须调 cover_jpeg_free 释放）
 * @return ESP_OK 成功；参数非法 / 解码失败 / 内存不足返回错误码
 */
esp_err_t cover_jpeg_decode(const uint8_t *jpeg, uint32_t len, lv_image_dsc_t *out);

/**
 * @brief 释放 cover_jpeg_decode 分配的图片（安全；img 可为 NULL 或全零）
 */
void cover_jpeg_free(lv_image_dsc_t *img);

#ifdef __cplusplus
}
#endif
