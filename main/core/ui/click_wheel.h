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
 *     内容总高度的 20%（即 **5 圈滑到底**）；
 *   - **行对齐步进**：内部按连续像素量累加，但每帧只把**行高的整数倍**
 *     交给列表，因此行槽在屏幕上的位置（含当前小数相位）保持不变，转动时是
 *     "内容一格一格换"的阶梯式翻行（见 cw_scroll_by_deg）。步进是**相对
 *     列表当前位置**的：不做"对齐到整行网格"的吸附，列表停在哪儿就从哪儿
 *     一格格跳；滚到顶/底时多余的滚动量由列表自己 clamp（正好停在第一项
 *     贴顶 / 最后一项贴底）。
 *     ⚠ 附带收益：位移由"格数"决定、与帧数无关（掉帧不累积成位移损失），
 *     且帧变慢时只表现为"多跳几格"而非突进 → 快速滚动原本的卡顿感在体感上
 *     消失。只改观感、不省绘制开销（详见 CLICK_WHEEL_USAGE.md 2.3.2）；
 *   - 直接点击按钮（按住期间没有转动）：弹出使用说明 Toast。
 *
 * ⚠ 按钮创建在**当前屏幕** `lv_scr_act()` 上（主界面 tabview 的兄弟，比
 *   tabview 晚创建 → 位于整个主界面之上），并清除自身的滚动链
 *   （SCROLL_CHAIN_HOR/VER），因此：
 *     - 拖拽按钮不会把主界面 tabview 的左右翻页手势劫走
 *       （否则 LVGL 会因 SCROLL_CHAIN 找到可滚动的 tabview，把按压
 *       判定为翻页并给按钮发 PRESS_LOST，画圈直接失效）；
 *     - 屏幕层对象不随页面横向滑动，按钮位置稳定。
 *   ⚠ **不要改挂 `lv_layer_top()`**：那样按钮会浮在下拉控制中心之上
 *   （Z 序 Bug），而"把控制中心面板也挂顶层"的修法会破坏 LVGL 的覆盖判定
 *   （`lv_refr_get_top_obj()` 只在当前屏幕的子对象树里查找覆盖绘制块的不
 *   透明对象），使被面板遮住的页面内容每帧重绘，下拉时严重掉帧
 *   （见 DONE_PLANS.md 第 35/38 点）。与面板同处屏幕层时，Z 序由创建顺序
 *   保证：`main_ui_create()`（本按钮）早于 `notification_panel_create()`。
 *   面板由**创建顺序**天然位于按钮之上，无需任何显隐切换。
 *   显隐仍需由外部驱动：`click_wheel_set_tab()` 由主界面切换页面时调用，
 *   `click_wheel_set_overlay()` 由全屏覆盖层（设置菜单）开合时调用。
 *
 * ⚠ 角度换算：角度采用屏幕坐标系（x 右、y 下），
 *   angle = atan2f(y - cy, x - cx)，顺时针为正；每帧角度差归一化到
 *   (-180, 180] 以避免跨 0°/360° 跳变。连续滚动量 =
 *   角度差 × (内容总高度 × 0.2 / 360) 像素，累加进内部浮点累加器后
 *   **按行高取整**（只输出行高的整数倍，见上"行对齐步进"）。
 *   行高通过 virtual_list_get_item_height() 取得（故本组件只服务于
 *   virtual_list 列表，不依赖其内部结构）。
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
 * 创建在当前屏幕 `lv_scr_act()` 上（主界面之上），初始隐藏；创建后由
 * `click_wheel_set_tab()` 与 `click_wheel_set_list()` 驱动显隐与滚动目标。
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
 * 覆盖层会盖住按钮，但按钮若不隐藏仍会拦截该区域的触摸，
 * 故覆盖层打开时调用方需隐藏按钮。
 *
 * ⚠ 仅对**创建在 `lv_scr_act()` 上、且早于本按钮**的覆盖层必要（如设置
 *   菜单）。晚于本按钮创建的屏幕层覆盖层（下拉控制中心面板）天然位于
 *   按钮之上（遮挡 + 接管触摸），无需调用本接口。
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
