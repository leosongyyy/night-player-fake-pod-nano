#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config_slot_popup.h
 * @brief 配置槽位选择弹窗（NightPlayer「配置插槽」，见 config_slots.h）
 *
 * 与 EQ 预设弹窗（ui/eq_popup.c）同款式：全屏半透明遮罩 + 中央圆角面板 +
 * 纵向可滚动行卡片，点击空白处关闭、点击行即切换。
 *
 * 列表内容（行数 = config_store_slot_count()）：
 *   SAVE1 <该配置里的播放列表名>      ← 未保存过的槽位只显示 "SAVE1"
 *   SAVE2 <...>                      ← **当前活跃槽位**用黄色文字高亮
 *   ...
 * 点击一行 → 立即 config_slots_switch() 切换并应用该配置 → 关闭弹窗。
 *
 * ⚠ 所有函数必须在 LVGL 上下文（持锁）中调用。
 */

/**
 * @brief 打开/关闭配置槽位弹窗（首次调用时懒创建控件并挂在 anchor_page 上）
 *
 * @param anchor_page 挂载页面（播放设置页的 tab 页），仅首次需要
 */
void config_slot_popup_toggle(lv_obj_t *anchor_page);

/** @brief 弹窗当前是否显示 */
bool config_slot_popup_is_visible(void);

#ifdef __cplusplus
}
#endif
