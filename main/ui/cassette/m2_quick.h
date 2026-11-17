#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m2_quick.h
 * @brief M2 快捷控制中心：亮度/音量推子、蓝牙/无线开关、系统设置入口
 *
 * 同一份控件体由 M2 独立页与全局抽屉共用：`cas_m2_build_body()` 创建一套
 * 控件并把句柄登记到本模块，所有视图都从 cas_state_t 读状态，
 * 任一处改动经 cas_m2_refresh() 同步到全部视图（不会双份不同步）。
 */

/** 在 page root 下创建 M2 完整页（幂等） */
void cas_m2_create(lv_obj_t *parent);

/** 依据 cas_state_t 刷新所有已建控件体（页 + 抽屉） */
void cas_m2_refresh(void);

/**
 * 创建一套 M2 控件体（页与抽屉共用）。返回 cas_m2_view_t*，调用方无需
 * 关心内部字段；刷新统一由 cas_m2_refresh() 完成。
 */
void *cas_m2_build_body(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
