#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file default_cover.h
 * @brief 默认封面图引用（仿 system_font.h 的开关切换模式）
 *
 * 通过 Kconfig CONFIG_UI_COVER_FLASH 决定引用指向：
 *   - 开启：RGB565 真彩色版 default_cover_flash（core/image/default_cover_flash.c，
 *           460×460 ≈ 423KB；与屏幕像素格式一致，LVGL 渲染走快速路径，
 *           滑动翻页流畅，适合正式使用）
 *   - 关闭：2bit 索引色版 default_cover（core/image/default_cover.c，
 *           约 52KB 省 Flash，渲染需逐像素查调色板，滑动时略卡，适合调试）
 *
 * 界面代码统一用 DEFAULT_COVER_IMG 宏取默认封面，不要直接引用具体图片。
 */

/** 2bit 索引色版默认封面（始终编译，省空间调试用） */
extern const lv_image_dsc_t default_cover;

#ifdef CONFIG_UI_COVER_FLASH
/** RGB565 真彩色版默认封面（极速，条件编译） */
extern const lv_image_dsc_t default_cover_flash;
#endif

#ifdef CONFIG_UI_COVER_FLASH
#define DEFAULT_COVER_IMG (&default_cover_flash)
#else
#define DEFAULT_COVER_IMG (&default_cover)
#endif

#ifdef __cplusplus
}
#endif
