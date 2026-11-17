#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m1_player.h
 * @brief M1 磁带播放器（磁带仓 + 进度推子 + 五联琴键）
 *
 * 磁带主体是唯一位图（320×320 RGB565 const/Flash），手写字/三色条/卷轴/
 * 螺丝都已烘焙在图内（架构 F17），P0 不叠加贴纸或齿轮覆层。
 */

/** 在 page root 下创建 M1 内容（幂等） */
void cas_m1_create(lv_obj_t *parent);

/** 依据 cas_state_t 全量刷新（标签/推子/键位/光晕） */
void cas_m1_refresh(void);

#ifdef __cplusplus
}
#endif
