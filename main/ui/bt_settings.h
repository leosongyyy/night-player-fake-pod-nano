#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file bt_settings.h
 * @brief 蓝牙设置全屏页（2026-11）
 *
 * 入口：
 *   - 控制中心蓝牙按钮**长按** → 先弹「蓝牙快速面板」（`bt_quick_panel`：
 *     设备列表点击连接/断开 + 本页入口），再从这里进入完整设置；
 *   - 设置菜单 →「蓝牙」→ 直接打开本页。
 *
 * 页面内容（三个视图，单页内切换）：
 *   1. 主视图：蓝牙开关 + 已保存设备（最多 5 个，**点击进入设备详情**，
 *      不在此切连接状态）+「配对新设备」；
 *   2. 设备详情：名称/MAC、当前连接状态、该设备音量、EQ 选择、安静模式
 *      开关（定时关机不断电、只保留蓝牙）、连接/断开、删除设备；
 *   3. 扫描视图：经典蓝牙扫描结果（名称/MAC/RSSI），点击配对并连接。
 *
 * 数据全部来自 bt_service / bt_store 的轮询（500ms），不注册跨任务回调，
 * 避免在协议栈任务里碰 LVGL。
 */

/** @brief 打开蓝牙设置页（懒创建；已打开时置前） */
void bt_settings_show(void);

/** @brief 关闭蓝牙设置页 */
void bt_settings_hide(void);

/** @brief 当前是否显示 */
bool bt_settings_is_visible(void);

#ifdef __cplusplus
}
#endif
