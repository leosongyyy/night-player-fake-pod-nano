#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file bt_quick_panel.h
 * @brief 控制中心长按蓝牙按钮弹出的「蓝牙快速面板」（2026-11）
 *
 * 与 `bt_settings` 的分工：
 *   - 本面板是**简易对话框**：只有已保存设备列表（点击 = 连接/断开）和
 *     底部「蓝牙设置」入口，适合从控制中心快速切换耳机；
 *   - `bt_settings` 是完整设置页：点击设备进入设备详情（连接/断开、EQ、
 *     睡眠免打扰、删除设备），另有配对扫描。
 *
 * 面板挂在 `lv_layer_top`，点击卡片外的黑色遮罩关闭；状态 500ms 轮询刷新。
 */

/** @brief 打开快速面板（懒创建；已打开时置前） */
void bt_quick_panel_show(void);

/** @brief 关闭快速面板 */
void bt_quick_panel_hide(void);

/** @brief 当前是否显示 */
bool bt_quick_panel_is_visible(void);

#ifdef __cplusplus
}
#endif
