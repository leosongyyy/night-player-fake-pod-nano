#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m4_clock.h
 * @brief M4 精密时钟：日期铭牌 + 3 组翻页卡 + 实时时钟/机械闹钟/秒表计时三 tab
 *
 * 翻页卡数字用 CAS_FONT_MONO 等宽不串位；实时时钟读真机 RTC（rtc_get_time），
 * 失败退化 "--"（Q4/U5）；秒表走时基于 esp_timer，启停/归零保持 cas_state_t.sw。
 */

/** 在 page root 下创建 M4 内容（幂等） */
void cas_m4_create(lv_obj_t *parent);

/** 依据 cas_state_t + RTC 全量刷新（翻页卡/三 tab/三个面板） */
void cas_m4_refresh(void);

#ifdef __cplusplus
}
#endif
