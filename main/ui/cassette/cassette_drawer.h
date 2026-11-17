#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "ui/cassette/cassette_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_drawer.h
 * @brief 全局下拉快捷抽屉（覆盖 7 屏，屏幕层，不挂 lv_layer_top()）
 *
 * F11：抽屉是**全屏不透明**覆盖层，必须挂在 `lv_scr_act()` 屏幕树里，
 * 才能被 LVGL 的覆盖判定识别、下层整页不参与重绘；半透明 Toast 才挂顶层。
 */

/** 创建抽屉；parent 应为 cas_root（屏幕层），最后创建以位于最上 */
void cas_drawer_create(lv_obj_t *parent);

void cas_drawer_toggle(void);
void cas_drawer_open(void);
void cas_drawer_close(void);
bool cas_drawer_is_open(void);

/** 同步抽屉中 7 个导航按钮的高亮态（由 cas_nav_show 调用） */
void cas_drawer_highlight(cas_page_id_t id);

/* M2 控件体由 m2_quick.h 导出 cas_m2_build_body()；抽屉在 T03 已接入真实
   控件体（不再有 T02 占位 label），页与抽屉共用同一 cas_state_t。 */

#ifdef __cplusplus
}
#endif
