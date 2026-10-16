#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file now_playing.h
 * @brief 正在播放页（四页滑动主界面的第 3 页）
 *
 * 布局（全屏 460x460）：
 *   - 封面图：460x460 全屏（目前用默认封面 default_cover，后续接入读取）；
 *     点击封面切换标题栏/控制栏的显示与隐藏；
 *   - 播放页底色为深灰色（非纯黑），封面隐藏时露出，增加层次感；
 *   - 顶部标题栏（50% 半透明黑，状态栏下方）：大字体歌曲标题 + 小字体
 *     专辑/艺术家（暂用播放列表名代替），超长文本跑马灯（非当前页关闭）；
 *     点击标题栏 → 播放列表更新为正在播放的目录并跳转到播放列表页；
 *   - 底部控制栏（50% 半透明黑）：当前时长 + 上一曲/播放暂停/下一曲 +
 *     总时长 + 进度条（拖动松手 seek）。
 *
 * 数据均来自 player_controller_get_status()，由内部 timer 周期刷新。
 */

/**
 * 滚动翻页时是否隐藏封面图（编译期配置宏，仅此一处，不占用 Kconfig）：
 * - 1：滑动开始时封面立即隐藏、滚动完全停止且停在播放页时 0.3s 渐显——
 *   翻页期间不绘制大图，带宽开销最小（经实测流畅无掉帧）；
 * - 0（当前采用）：封面始终显示，仅把滚动期间标题栏/控制栏/状态栏改为
 *   不透明纯黑（不做 alpha 混合）、停止后渐变回半透明（经实测同样流畅）。
 * 两种模式下滚动期间各栏都会变为不透明纯黑以省带宽。
 */
#define NP_HIDE_COVER_ON_SCROLL  0

/**
 * @brief 创建正在播放页（tabview 的一个 tab）
 * @param tv lv_tabview 对象
 * @return tab 页对象
 */
lv_obj_t *now_playing_create(lv_obj_t *tv);

/**
 * @brief 开关标题/副标题的跑马灯（页面不在可视区时关闭省电）
 * @param enable true 开启 / false 关闭（裁剪）
 */
void now_playing_set_marquee(bool enable);

/**
 * @brief 强制恢复标题栏/控制栏显示
 *
 * 由 main_ui 在离开正在播放页时调用：用户在播放页点封面隐藏的
 * 栏（以及联动的状态栏）在下次进入前复位显示。
 */
void now_playing_force_show_ui(void);

/**
 * @brief 放行首次封面加载（把首次加载推迟到整段开机 UI 创建完成之后）
 *
 * now_playing_create() 建好封面任务后会立刻请求加载当前歌曲封面，而这次加载
 * （song_meta_load + esp_jpeg）会与 app_ui_create 里 LVGL 对象的创建**并行**
 * 争抢内部 RAM —— 实测导致"热点对象钉内部 RAM"的钉住量每次开机相差约 10KB，
 * 帧率随之抖动（见 ideas_and_exps/IRAM_AUDIT_LVGL9.md §13.3）。
 * 由 ui/app.c 在 app_ui_create() 末尾调用本函数放行；此前若有加载请求会先挂起，
 * 放行时一次性补发。放行之后切歌照常实时加载，不受影响。幂等。
 */
void now_playing_enable_cover_load(void);

/**
 * @brief 释放封面加载任务（仅"重新扫描/重启前腾内部 RAM"场景使用）
 *
 * 删除后台封面解码任务并清空句柄，释放约 6KB 内部 RAM 栈；此后不再换
 * 封面（仅适用于**马上要 esp_restart** 的路径，见 ui/settings_menu.c 的
 * rescan_release_tasks）。幂等：任务未创建或已释放时安全返回。
 */
void now_playing_release_cover_task(void);

#ifdef __cplusplus
}
#endif
