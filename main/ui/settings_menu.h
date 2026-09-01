#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file settings_menu.h
 * @brief 全屏设置菜单（基于 LVGL 8 的 lv_menu，暗色主题）
 *
 * 由通知栏的"设置"按钮打开（收起通知栏后全屏覆盖）。菜单内容：
 *   - 睡眠定时器（子菜单：开机默认开启 + 清醒感知开关，底部说明清醒感知
 *     与休眠行为）
 *   - 屏幕自动息屏（7 档：10秒/15秒/30秒/1分钟/2分钟/5分钟/10分钟）
 *   - 按键帮助（亮屏/息屏两套按键映射说明）
 *   - 关于（作者信息）
 *   - 重新扫描（子页确认：是 = 停止播放、使已存播放列表数据无效并重启，
 *     开机重新扫描 SD 卡；否 = 返回上一级）
 *
 * 与设置页（settings_page，九宫格）共用同一 config_store 配置源；
 * 本菜单承载"低频/需要子页面"的设置项，设置页承载高频三项。
 */

/**
 * @brief 创建全屏设置菜单（初始隐藏）
 *
 * 需在通知栏创建之后调用（lv_scr_act() 最后创建 → 覆盖在最上层）。
 * 所有子页面与样式在创建时一次性构建，之后 show/hide 只切换可见性。
 */
void settings_menu_create(void);

/** @brief 显示菜单（重置到根页面；调用方需在 LVGL 任务/持锁上下文） */
void settings_menu_show(void);

/** @brief 隐藏菜单 */
void settings_menu_hide(void);

/** @brief 菜单当前是否显示 */
bool settings_menu_is_visible(void);

#ifdef __cplusplus
}
#endif
