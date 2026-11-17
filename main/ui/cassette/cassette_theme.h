#pragma once

#include "lvgl.h"

#include "core/fonts/cassette_font.h"
#include "core/fonts/system_font.h"
#include "core/hardware/display.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_theme.h
 * @brief 袖珍磁带机 UI 全局唯一色板 / 坐标换算 / 字体 / 共享样式
 *
 * S3 评审要求：15 个 Palette 色值严格取 Web 原型给定值，任何模块不得
 * 硬编码近似色；统一从本文件取宏。
 */

/** ---- 15 色 Palette（原型给定值，唯一出处） ---- */
#define CAS_COL_BG          lv_color_hex(0x121110)
#define CAS_COL_PANEL       lv_color_hex(0x1C1A17)
#define CAS_COL_METAL       lv_color_hex(0x3A3632)
#define CAS_COL_METAL_HI    lv_color_hex(0x5C554E)
#define CAS_COL_GROOVE      lv_color_hex(0x0A0908)
#define CAS_COL_PAPER       lv_color_hex(0xEFE6D5)
#define CAS_COL_KEYCAP      lv_color_hex(0xD8CFC0)
#define CAS_COL_AMBER       lv_color_hex(0xF5A623)
#define CAS_COL_AMBER_HI    lv_color_hex(0xFFD07B)
#define CAS_COL_STRIPE_Y    lv_color_hex(0xE8A838)
#define CAS_COL_STRIPE_O    lv_color_hex(0xD96B27)
#define CAS_COL_STRIPE_R    lv_color_hex(0x9E2A2B)
#define CAS_COL_FG          lv_color_hex(0xF4EFE6)
#define CAS_COL_INK         lv_color_hex(0x2B231D)
#define CAS_COL_DIM         lv_color_hex(0x9E9488)

/** Web 的 1cqw = 容器宽 1%；460 屏 → CAS_CQW(8.4) = 38。X=10 偏移由驱动处理。 */
#define CAS_CQW(x)          ((int)((x) * EXAMPLE_LCD_H_RES / 100))

/** 状态栏高度 8.4cqw（P0-1），同时是页内容区的上边界。 */
#define CAS_STATUS_H        CAS_CQW(8.4)

/** ---- 字体分工 ---- */
#define CAS_FONT_TITLE      CAS_FONT_CN              /* 机身丝印 / 菜单 / 标题 */
#define CAS_FONT_BODY       CAS_FONT_CN              /* 中文正文 */
#define CAS_FONT_NUM        CAS_FONT_MONO            /* 翻页钟 / 时长 / 容量 */
#define CAS_FONT_ICON       UI_ICON_TEXT_FONT        /* LV_SYMBOL_* + 中文混排 */
#define CAS_FONT_TINY       UI_SMALL_FONT            /* 状态栏数字 / 双行小字 */

/**
 * 共享样式工厂。样式对象在首次调用时初始化一次，返回只读指针；
 * 7 个模块用 `lv_obj_add_style(obj, cas_style_metal(), 0)` 复用，避免
 * 每页各自复制一份样式属性（F9 内存预算）。
 * ⚠ 首次调用必须发生在 LVGL 任务上下文（创建 UI 时满足）。
 */
const lv_style_t *cas_style_metal(void);     /* 拉丝枪灰金属：渐变 + 亮边 + 深槽 */
const lv_style_t *cas_style_groove(void);    /* 凹陷滑槽：深底 + 内阴影近似 */
const lv_style_t *cas_style_paper(void);     /* 米白纸面 label：纸底 + 墨色字 */

#ifdef __cplusplus
}
#endif
