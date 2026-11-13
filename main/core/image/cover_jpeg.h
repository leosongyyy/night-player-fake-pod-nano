#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JPEG 封面解码（ESP32-S31 硬件 JPEG 解码器 → 失败退回 esp_jpeg/tjpgd）
 *
 * 上位机 .npd 里存放的是 460x460 的 JPEG（黑底居中），本模块将其解码为
 * RGB565 像素缓冲并封装成 lv_image_dsc_t，可直接 lv_image_set_src 显示。
 *
 * 两条解码路径（`CONFIG_UI_COVER_HW_JPEG` 控制，见 main/Kconfig.projbuild）：
 *   - **硬件（默认）**：esp_driver_jpeg，DMA2D 直写 PSRAM，460x460 实测
 *     10ms 量级；引擎懒创建一次后常驻（只占几 KB 内部 RAM）。
 *     详细设计与踩坑见 ideas_and_exps/COVER_JPEG_HW.md。
 *   - **软件（兜底）**：esp_jpeg/tjpgd，460x460 约 100~300ms。硬件路径创建
 *     失败或单次解码失败时自动走这条，封面不会因为硬件出问题而消失。
 * 两条路径的输出格式完全一致：RGB565 小端、**紧凑 stride**（= 宽 × 2B）。
 *
 * ⚠ 即使走硬件，解码也在毫秒级但仍需放到后台任务：调用点必须
 *   在 cover_load 任务（Core1）里，完成后持 LVGL 锁再更新 UI，
 *   禁止在 LVGL 任务/回调中直接调用。
 *
 * ⚠ RGB565 字节序（LVGL 9 迁移后）：输出为**本机小端** RGB565，
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
