#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file list_common.h
 * @brief 列表页公共 UI 组件（页面样式 + 顶部标题栏 + 深灰卡片背景）
 *
 * 供目录列表页（dir_list）与播放列表页（playlist_page）复用：
 *   - ui_page_style()：tab 页通用样式（黑底、无边框、无内边距、不可滚动）
 *   - ui_title_bar_create() / ui_title_bar_set_text()：顶部标题栏（高 44，
 *     左图标(可选) + 标题 + 右侧可选灰色计数文字，标题超长自动裁剪，
 *     底部细分隔线），文字可动态更新；背景透明（深灰卡片色由页面提供）
 *   - ui_title_bar_set_icon()：为标题栏添加/更新左侧 LV_SYMBOL 图标
 *   - ui_card_bg_create()：创建"上圆下直"的深灰卡片背景（列表页用）
 *   - UI_CARD_COLOR：卡片底色（0x1E1E1E，与设置菜单/播放页底色一致）
 */

/** 标题栏高度（像素）：列表内容从它下方开始 */
#define UI_TITLE_BAR_H  44

/** 顶部状态栏高度（像素）：主界面容器撑满全屏（460x460），各页面内容
    从它下方开始（沉浸式布局预留）；状态栏本身、通知栏下拉触发区共用。
    改动只需改这里。 */
#define UI_STATUS_BAR_H  46

/** 深灰卡片底色：黑背景上的层次色（同设置菜单行卡/播放页底色 0x1E1E1E） */
#define UI_CARD_COLOR   lv_color_hex(0x1E1E1E)

/** 列表页左右缩进（px）：卡片两侧距屏幕边缘的黑边（含设置页同款留白） */
#define UI_CARD_MARGIN_X  15

/** 列表行高度（px）：**目录页与歌曲页必须一致**（历史遗留：目录页曾单独用 52，
    导致两页行数/行距/滚动步进都不统一）。
 *  下限约束：虚拟列表给文本留 12px（上下各 6）的居中余量，故
 *  `UI_LIST_ITEM_H - 12` 必须 ≥ 系统字体 line_height（完整字体 33 / 精简字体约 20），
 *  取 56 对两套字体都成立（见 VIRTUAL_LIST_USAGE.md 踩坑 5）。
 *  ⚠ 它同时决定 `virtual_list` 的行槽数与 pad_top，改它等于改两页的可见行数。 */
#define UI_LIST_ITEM_H  56

/**
 * @brief 应用 tab 页通用样式（黑底、无边框、无内边距、不可滚动）
 * @param page tab 页容器（lv_tabview_add_tab 的返回值）
 */
void ui_page_style(lv_obj_t *page);

/**
 * @brief 创建顶部标题栏（高 UI_TITLE_BAR_H，底部 1px 灰线分隔，背景透明）
 * @param parent 父容器（tab 页）
 * @param title  标题文字（白色，超长自动裁剪）
 * @param sub    右侧计数/说明文字（灰色，可为 NULL）
 * @return 标题栏对象（后续用 ui_title_bar_set_text/…_set_icon 更新）
 */
lv_obj_t *ui_title_bar_create(lv_obj_t *parent, const char *title,
                              const char *sub);

/**
 * @brief 更新标题栏文字
 * @param bar    ui_title_bar_create 返回的标题栏对象
 * @param title  新标题（NULL 表示不变）
 * @param sub    新计数文字（NULL 表示隐藏右侧文字；首次非 NULL 会自动创建）
 */
void ui_title_bar_set_text(lv_obj_t *bar, const char *title, const char *sub);

/**
 * @brief 设置标题栏左侧的 LV_SYMBOL 图标（与标题同行，标题自动右移）
 * @param bar   ui_title_bar_create 返回的标题栏对象
 * @param icon  图标字符串（LV_SYMBOL_*，用 LVGL 内置字体绘制；
 *              NULL/空串 = 无图标，标题回到最左）
 */
void ui_title_bar_set_icon(lv_obj_t *bar, const char *icon);

/**
 * @brief 创建"上圆下直"深灰卡片背景（列表页主体）
 *
 * 实现（叠色技巧）：先画一张整卡圆角矩形（radius=corner，顶部圆角轮廓
 * 由此显现），再在其上叠一张同色直角矩形、覆盖卡片下半部（高 2*corner，
 * 含底部两角圆角缺口）——圆角与直角两控件同色无缝，视觉为
 * 「左上/右上圆角 + 底部直角」的一条卡片，与背后黑色形成层次。
 *
 * ⚠ 仅纯背景：不接收事件、不参与滚动，子内容（标题栏/虚拟列表）需在
 * 调用本函数之后创建，自然叠在背景之上。
 *
 * @param parent 父容器（页面，黑底）
 * @param x,y    卡片区域左上角
 * @param w,h    卡片区域宽高
 * @param corner 顶部圆角半径（px），同时决定直角覆盖层高度 = 2*corner
 */
void ui_card_bg_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_coord_t corner);

#ifdef __cplusplus
}
#endif
