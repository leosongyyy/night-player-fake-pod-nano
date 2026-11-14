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
 *   - 虚拟 ClickWheel 选项（开启 / 滑动方向 / 速度三档）
 *   - 时间与日期（子菜单：设置日期 / 时间设置 / WiFi对时 / 时区设置 /
 *     时间格式；日期、时间、时区用滚轮弹窗修改；WiFi对时读 SD 卡 wifi.txt 连 WiFi
 *     做 NTP，全屏进度 + 取消。见 ui/time_date_popup.c、ui/time_sync_ui.c）
 *   - 电源选项（**占位空页**，本轮先留入口，后续会话继续实现）
 *   - 按键帮助（亮屏/息屏两套按键映射说明）
 *   - 关于（作者信息；页面最下方为"开发者选项"入口子页：电池电压日志
 *     开关与采样间隔档位、触摸测试白点开关——均为 NVS 运行时开关，
 *     开机检查、修改需重启生效，附"立即重启生效"按钮）
 *   - 重新扫描（子页确认：是 = 停止播放、使已存播放列表数据无效并重启，
 *     开机重新扫描 SD 卡；否 = 返回上一级）
 *
 * 与设置页（settings_page，九宫格）共用同一 config_store 配置源；
 * 本菜单承载"低频/需要子页面"的设置项，设置页承载高频三项。
 *
 * ⚠ **懒加载（2026-09）**：菜单不再随开机创建——整个菜单常驻**实测约 29KB
 * 内部 RAM（= DMA 池）、构建耗时约 310ms**，全部从开机路径移除（开机主界面
 * 因此提前约 305ms 出现）。**首次 settings_menu_show()** 时才建"外壳 + 根页面"
 * （实测 44ms），6 个子页（含"关于"里的开发者选项页）在首次点进时各建一次。
 * 因此本模块没有"创建"接口，打开菜单一律调用 settings_menu_show()。
 * 层级关系不变：菜单晚于状态栏/通知栏/ClickWheel 创建，仍位于屏幕层最上。
 */

/** @brief 显示菜单（首次调用会懒创建整个菜单外壳；调用方需在 LVGL 任务/持锁上下文） */
void settings_menu_show(void);

/** @brief 隐藏菜单 */
void settings_menu_hide(void);

/** @brief 菜单当前是否显示 */
bool settings_menu_is_visible(void);

#ifdef __cplusplus
}
#endif
