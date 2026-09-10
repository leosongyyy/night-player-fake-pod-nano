#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file click_wheel.h
 * @brief 虚拟 ClickWheel 大列表速滑按钮（见 FUTURE_PLAN.md 第 24 点）
 *
 * 为长列表提供"转圈翻页"的快速滚动交互：
 *   - 主界面右下角悬浮一个黄色圆形按钮（仅当列表条目超过阈值时显示）；
 *   - 按住按钮后在屏幕上画圈：以**屏幕中心**为圆心计算手指角度，
 *     顺时针（角度增大）向下滚、逆时针向上滚，转满一圈滚动
 *     内容总高度的 10%；
 *   - 直接点击按钮（按住期间没有转动）：弹出使用说明 Toast。
 *
 * ⚠ 按钮创建在 `lv_layer_top()`（系统最顶层），并清除自身的滚动链
 *   （SCROLL_CHAIN_HOR/VER），因此：
 *     - 拖拽按钮不会把主界面 tabview 的左右翻页手势劫走
 *       （否则 LVGL 会因 SCROLL_CHAIN 找到可滚动的 tabview，把按压
 *       判定为翻页并给按钮发 PRESS_LOST，画圈直接失效）；
 *     - 顶层对象不随页面横向滑动，按钮位置稳定；
 *     - 与 Toast 同层共存（Toast 后创建则显示在按钮之上）。
 *   因为挂在顶层，**显隐需由外部驱动**：`click_wheel_set_tab()` 由
 *   主界面切换页面时调用，`click_wheel_set_overlay()` 由全屏覆盖层
 *   （设置菜单）开合时调用。
 *
 * ⚠ 角度换算：角度采用屏幕坐标系（x 右、y 下），
 *   angle = atan2f(y - cy, x - cx)，顺时针为正；每帧角度差归一化到
 *   (-180, 180] 以避免跨 0°/360° 跳变。滚动量 =
 *   角度差 × (条目总数 × 行高 × 0.1 / 360) 像素。
 *
 * ⚠ 触摸坐标取自 lvgl_port 的触摸轮询（与 LVGL 输入设备同一份数据，
 *   已做 29/30 校准），因此按住期间手指可移出按钮范围，画大圈不受限。
 * ⚠ 所有接口都需在持有 LVGL 锁（lvgl_port_lock）时调用。
 */

/** 支持 ClickWheel 的页面数（目录页 tab0 / 歌曲页 tab1） */
#define CLICK_WHEEL_TAB_COUNT  2

/**
 * @brief 创建 ClickWheel 悬浮按钮（整个应用只需创建一次）
 *
 * 创建在 `lv_layer_top()` 上，初始隐藏；创建后由 `click_wheel_set_tab()`
 * 与 `click_wheel_set_list()` 驱动显隐与滚动目标。
 *
 * @return 按钮对象句柄（NULL = 创建失败）
 */
lv_obj_t *click_wheel_create(void);

/**
 * @brief 设置某个页面（tab）对应的滚动列表
 *
 * 页面构建/重建列表后调用；传 NULL 表示该页面当前没有可滚动的列表
 * （如"该目录暂无歌曲"），按钮自动隐藏。
 *
 * @param tab         页面下标（0=目录页，1=歌曲页；越界忽略）
 * @param list        虚拟列表对象（virtual_list_create 返回值）
 * @param item_count  该列表条目总数（决定是否显示，≤ 阈值时隐藏）
 */
void click_wheel_set_list(uint8_t tab, lv_obj_t *list, uint32_t item_count);

/**
 * @brief 通知当前主界面页面（tab 下标）：按钮据此决定是否显示
 *
 * 由主界面切页时调用（`main_tabs_sync`）。仅目录页/歌曲页显示。
 *
 * @param tab 当前页面下标（0..3；非列表页按钮隐藏）
 */
void click_wheel_set_tab(uint8_t tab);

/**
 * @brief 通知全屏覆盖层（设置菜单等）是否显示：显示时按钮隐藏
 *
 * 覆盖层在顶层之下会盖住按钮，但按钮在顶层仍会拦截该区域的触摸，
 * 故覆盖层打开时必须隐藏按钮。
 *
 * ⚠ 只适用于创建在 `lv_scr_act()` 上的覆盖层（设置菜单）。创建在
 *   `lv_layer_top()` 上、且**晚于本按钮**创建的覆盖层（下拉控制中心）
 *   天然位于按钮之上（遮挡 + 接管触摸），无需调用本接口。
 *
 * @param visible true = 覆盖层显示中（按钮隐藏）
 */
void click_wheel_set_overlay(bool visible);

/**
 * @brief 启用/禁用速滑功能（NVS 配置 click_wheel，默认启用）
 *
 * 关闭时按钮立即隐藏（并终止进行中的画圈、收起指示圆点），无论列表
 * 长度/页面/覆盖层状态如何都不再显示。调用方：
 *   - `main_ui_create()` 创建后按 `config_store_get_click_wheel()` 初始化；
 *   - 设置菜单「虚拟 ClickWheel」行切换后立即调用（无需重启）。
 *
 * @param on true = 启用；false = 禁用
 */
void click_wheel_set_enabled(bool on);

#ifdef __cplusplus
}
#endif
