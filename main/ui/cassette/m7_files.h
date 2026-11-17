#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m7_files.h
 * @brief M7 文件管理：容量条 + 面包屑 + 静态目录树 5 行 + 选中/新建(示例)/详情
 *
 * 全部静态示例数据（保留「（示例）」），P0 不读真实 FS、不落盘；
 * 面包屑/选中/详情展开走 cas_state_t.path/sel/detail。
 */

/** 在 page root 下创建 M7 内容（幂等） */
void cas_m7_create(lv_obj_t *parent);

/** 依据 cas_state_t 全量刷新（面包屑/文件行/详情面板/三键） */
void cas_m7_refresh(void);

#ifdef __cplusplus
}
#endif
