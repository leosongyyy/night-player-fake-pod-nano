#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file settings_page.h
 * @brief 播放设置页（四页滑动主界面的第 4 页）
 *
 * 九宫格布局（3×3，预留空位），按钮默认无边框无背景、仅按下时黄色背景，
 * 图标与文字均随状态切换：
 *   - 播放模式：图标随模式切换（循环↻ / 单曲↻1 / 列表☰），文字直接显示
 *     当前循环方式（全部/单曲/列表循环），点击轮换（NVS 持久化）
 *   - 顺序/随机播放：图标（▶/⇄）与文字（顺序/随机播放）随开关切换，
 *     点击切换并持久化（随机算法后续接入播放引擎）
 *   - 睡眠定时："Zz" 图标；未定时显示 OFF，定时中每秒刷新剩余时间
 *     "分:秒"；点击在 15/30/45/60/90/120/180/240 分钟间轮换并保存为
 *     默认时长；长按 关闭/切到默认时长（初始默认 45 分钟）
 */

/**
 * @brief 创建播放设置页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *settings_page_create(lv_obj_t *tv);

#ifdef __cplusplus
}
#endif
