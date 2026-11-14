#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file eq_popup.h
 * @brief EQ 预设选择弹窗（播放设置页第 7 格"EQ"入口，FUTURE_PLAN 第 10 点）
 *
 * 点击设置页九宫格左下角的「EQ」格弹出：顶部标题 + 选项列表：
 *   - 第一项"无 EQ（关闭）"；
 *   - 之后为扫描 /sdcard/.musicfx 得到的各 .eq 文件名（每项点击即解析应用）；
 *   - 当前生效项黄色高亮；点击列表外区域关闭弹窗。
 *
 * 选择处理（全部在 LVGL 事件回调内执行，已持锁）：
 *   - 无 EQ → eq_apply(NULL) + 持久化清除，Toast「EQ 已关闭」；
 *   - .eq 文件 → eq_parse_named 解析：成功则 eq_apply + config_store_set_eq
 *     （参数入 NVS）+ Toast 反馈；失败（段数超 10 / 内容无效 / 打不开）按
 *     错误类型弹 Toast，弹窗保持打开供换选其它预设。
 *
 * 弹窗挂在锚点页面（settings_page 的 tab 页）上：翻页即随页面移出屏幕
 * （等价关闭），无需额外收尾。
 *
 * ⚠ 调用方须持 LVGL 锁（事件回调内已持锁，可直接调用）。
 */

/**
 * @brief 打开/切换 EQ 选择弹窗（已显示则关闭）
 * @param anchor_page 兼容参数（弹窗固定挂在 lv_layer_top()，此参数可传 NULL）
 */
void eq_popup_toggle(lv_obj_t *anchor_page);

/**
 * @brief 为某个蓝牙设备选择 EQ（2026-11）
 *
 * 与全局 EQ 的区别：选择结果写进 bt_store 的设备记录（而不是 config_store），
 * 且只有该设备是**当前连接设备**时立即 eq_apply；断开后自动恢复配置槽位 EQ。
 *
 * @param anchor_page 兼容参数（可 NULL）
 * @param dev_idx     bt_store 槽位下标（0~4）
 */
void eq_popup_toggle_for_device(lv_obj_t *anchor_page, int dev_idx);

/** @brief 弹窗当前是否显示 */
bool eq_popup_is_visible(void);

#ifdef __cplusplus
}
#endif
