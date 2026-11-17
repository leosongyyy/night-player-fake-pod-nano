#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file m6_settings.h
 * @brief M6 系统设置：均衡器分段 / 机械按键音 / 背光常亮 / 休眠旋钮4档 / 关于铭牌
 *
 * 末行含「切换到经典界面（示例）」隐藏项：点击写 NVS ui_mode=LEGACY 并重启，
 * 对应 L1 应急回退的反向入口（架构 §1.4b）；从经典界面切回用开机 A+B 3s
 * （cassette_boot_combo_check() 双向 toggle）。所有状态走 cas_state_t。
 */

/** 在 page root 下创建 M6 内容（幂等） */
void cas_m6_create(lv_obj_t *parent);

/** 依据 cas_state_t 全量刷新（分段/拨杆/旋钮/文本） */
void cas_m6_refresh(void);

#ifdef __cplusplus
}
#endif
