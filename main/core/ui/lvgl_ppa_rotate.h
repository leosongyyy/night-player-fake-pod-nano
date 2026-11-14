/**
 * @file lvgl_ppa_rotate.h
 * 实验性：用 PPA（ESP32-S31 的 2D 像素加速器）把 LVGL 画面顺时针旋转
 * 0/90/180/270°，并支持运行时切换。
 *
 * 背景与设计取舍见 ideas_and_exps/PPA_ROTATE.md。要点：
 *   · **不用** `lv_display_set_rotation()`：LVGL 9 的软件旋转是在绘制管线里
 *     用矩阵逐 draw task 变换，S3 时代实测帧率暴跌；本方案让 LVGL 照旧按
 *     460x460 竖屏渲染，只在 flush 回调里把"整块绘制缓冲"交给 PPA 的 SRM
 *     （缩放-旋转-镜像）硬件旋转后，再按旋转后的坐标提交给面板。
 *   · 触摸坐标由 LVGL 移植层的 read_cb 按同一角度做**逆变换**（画面顺时针
 *     转 θ，触摸就逆时针转 θ），再按左右手做 29/30 缩放校准；本模块只管画面。
 *   · 每块输出缓冲与 LVGL 的双缓冲一一对应：LVGL 在 `draw_buf_flush()` 里
 *     会先等上一块传输完成（`wait_for_flushing`）再调 flush_cb，所以**单块**
 *     旋转输出缓冲就够用（flush_cb 返回时没有 DMA 正在读它）。
 *   · 旋转缓冲在初始化时**总是**分配（即使当前角度是 0），这样运行时切换
 *     不需要在开机末期碎片化的内部 RAM 里再申请 41KB。
 *
 * 由 CONFIG_UI_DISPLAY_ROTATE_PPA 控制编译；关闭时所有函数退化为空实现/
 * 返回 false，调用点在编译期也被裁掉，无任何运行时开销。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 PPA 旋转（注册 SRM client + 分配旋转输出缓冲 + 方向自检）
 *
 * 必须在 LVGL 显示对象与绘制缓冲创建之后、LVGL 任务开始渲染之前调用一次。
 * 自检会验证 90/180/270 三个角度的坐标映射（PPA 的"逆时针"约定与图像坐标
 * 的对应关系没有明文，写反了只有上屏才发现）。
 *
 * @param panel_handle    面板句柄（旋转后的 tile 由本模块直接提交给面板）
 * @param hor_res         逻辑屏幕宽（旋转前，LVGL 的宽度，如 460）
 * @param ver_res         逻辑屏幕高（旋转前，LVGL 的高度，如 460；本实验只支持方形）
 * @param max_tile_bytes  LVGL 单块绘制缓冲的最大字节数（旋转前后 tile 像素数
 *                        相同，所以旋转输出缓冲取同样大小即可）
 * @param initial_cw_deg  开机使用的顺时针角度：0 / 90 / 180 / 270（来自 NVS）
 * @return true  旋转路径可用（当前角度可能仍为 0 = 关闭）
 * @return false 未启用 / 分配失败 / PPA 自检失败（调用方按原样刷屏）
 */
bool lvgl_ppa_rotate_init(esp_lcd_panel_handle_t panel_handle,
                          uint32_t hor_res, uint32_t ver_res,
                          size_t max_tile_bytes, uint16_t initial_cw_deg);

/**
 * @brief 运行时切换旋转角度（顺时针 0/90/180/270）
 *
 * 只改本模块的状态；调用方随后必须让整屏重绘（见 lvgl_port_set_display_rotation），
 * 否则旧方向的残留块会和新块混在一起。必须在 LVGL 任务上下文/持锁时调用
 * （与 flush 回调同线程，避免读写竞争）。
 *
 * @return true 已切换；false 未初始化/角度非法/该角度自检不支持
 */
bool lvgl_ppa_rotate_set_angle(uint16_t cw_deg);

/** @brief 当前生效的顺时针角度（0/90/180/270；未初始化时为 0） */
uint16_t lvgl_ppa_rotate_get_angle(void);

/** @brief PPA 旋转是否可用（初始化成功；与当前角度是否为 0 无关） */
bool lvgl_ppa_rotate_available(void);

/**
 * @brief 尝试把一块 LVGL 绘制缓冲按当前角度旋转并提交给面板
 *
 * @param area   LVGL 本次刷新区域（未旋转的坐标系，含端点）
 * @param px_map 该区域的 RGB565 紧凑缓冲（行宽 = area 宽度，LVGL PARTIAL 模式
 *               下 `layer->draw_buf->data` 就是这种紧凑布局）
 * @return true  已处理（旋转 + 字节序交换 + 提交），调用方**不要**再走未旋转路径
 * @return false 未启用 / 角度为 0 / 失败；调用方回退到原来的"就地 swap + 原坐标提交"
 */
bool lvgl_ppa_rotate_flush(const lv_area_t *area, uint8_t *px_map);

#ifdef __cplusplus
}
#endif
