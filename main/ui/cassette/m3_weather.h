#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m3_weather.h
 * @brief M3 天气盘：米白纸盘 + 三色条 + 大号温度 + 程序化气象徽章 + 三表 + 三日切换
 *
 * 气象徽章（晴转多云/雨/晴）全部用 lv_obj 圆 + lv_line 程序化绘制（工程无 SVG），
 * 不新增位图；数据为静态「（示例）」，三日切换只改 cas_state_t.weather_day。
 */

/** 在 page root 下创建 M3 内容（幂等） */
void cas_m3_create(lv_obj_t *parent);

/** 依据 cas_state_t 全量刷新（城市/温度/徽章/三键/三表） */
void cas_m3_refresh(void);

#ifdef __cplusplus
}
#endif
