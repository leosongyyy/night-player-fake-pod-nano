#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m5_list.h
 * @brief M5 播放列表：5 行交错曲目 + 当前曲 LED/均衡器 + 底部四键 + 甲乙面切换
 *
 * 数据为静态「（示例）」，四键改 cas_state_t（track_idx/play_mode/side），
 * 甲乙面切换同步状态栏标题；真实切歌属 P1 挂点。
 */

/** 在 page root 下创建 M5 内容（幂等） */
void cas_m5_create(lv_obj_t *parent);

/** 依据 cas_state_t 全量刷新（行高亮/LED/四键/标题） */
void cas_m5_refresh(void);

#ifdef __cplusplus
}
#endif
