#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file default_cover.h
 * @brief 默认封面图（460x460，2bit 索引色）
 *
 * 数据在 core/image/default_cover.c（lv_img_conv 生成的 LVGL 图片描述符）。
 * 播放页面在未读取到真实封面时显示此图；后续接入封面读取功能后替换。
 */
extern const lv_img_dsc_t default_cover;

#ifdef __cplusplus
}
#endif
