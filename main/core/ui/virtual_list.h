#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file virtual_list.h
 * @brief 通用虚拟滚动列表组件（基于 LVGL 8.4）
 *
 * 针对"数据量大、条目外观高度一致（仅文本与颜色不同）"的列表场景：
 * 只创建固定数量的 cell（= 可见数 + 2 个缓冲）复用渲染，滚动时把滚出
 * 视口的 cell 重新绑定到新的条目上，从而用极少的控件承载海量数据。
 *
 * 特性：
 *   - 虚拟滚动 + LVGL 原生滚动条（靠透明占位对象撑开真实内容高度）
 *   - 字符串/颜色由外部绑定回调动态解析（数据源不依赖具体格式）
 *   - cell 锚定在内容坐标（index × item_height），滚动时只有滚出/滚入视口的
 *     cell 被回收重绑，常驻 cell 的位置/文本/颜色/跑马灯全程不被触碰
 *   - 支持滚动到指定下标、刷新重渲染、点击选中与高亮
 *   - 样式写死在本组件内（整个项目仅目录/歌曲两处列表复用）
 *
 * ⚠ 依赖 `CONFIG_LV_USE_LARGE_COORD=y`（lv_coord_t 升级为 int32_t）：
 *   真实内容高度 = 条目总数 × 条目高度，会超过 int16 坐标上限（±32767），
 *   只有开启大坐标后才能撑开真实高度以驱动原生滚动条。
 *
 * 需在持有 LVGL 锁（lvgl_port_lock）时调用。
 */

/**
 * @brief 条目绑定回调
 *
 * 组件在 cell 需要填充/重用时调用；由调用方根据条目原始数据解析出
 * 文本与颜色并设置到 cell 上。
 *
 * @param cell      待填充的控件（本组件内部为 lv_label，文本/颜色都设到它）
 * @param index     条目下标（0 .. item_count-1）
 * @param item      条目原始数据指针（= data + index * item_size）
 * @param user_data 创建列表时传入的上下文（存放获取字符串所需的内容，
 *                  如字符串池指针、高亮判定所需状态等）
 */
typedef void (*virtual_list_bind_cb_t)(lv_obj_t *cell, uint32_t index,
                                       const void *item, void *user_data);

/**
 * @brief 选中回调：用户点击某条并选中后触发
 *
 * @param index     被选中的条目下标
 * @param user_data 创建列表时传入的上下文
 */
typedef void (*virtual_list_selected_cb_t)(uint32_t index, void *user_data);

/**
 * @brief 创建虚拟滚动列表
 *
 * @param parent      父容器
 * @param w, h        列表尺寸（宽、高，像素）
 * @param item_height 每条固定高度（像素）
 * @param item_count  条目总数
 * @param data        条目数据首地址（连续存储，每条 item_size 字节）
 * @param item_size   单条数据的字节数
 * @param bind_cb     条目绑定回调（负责填充文本与颜色，不可为空）
 * @param user_data   传给绑定/选中回调的上下文
 *
 * @return 列表对象（lv_obj_t*），后续各接口用它作为句柄
 */
lv_obj_t *virtual_list_create(lv_obj_t *parent,
                              lv_coord_t w, lv_coord_t h,
                              lv_coord_t item_height,
                              uint32_t item_count,
                              const void *data, uint32_t item_size,
                              virtual_list_bind_cb_t bind_cb,
                              void *user_data);

/**
 * @brief 滚动到指定条目（顶部对齐）
 *
 * @param list  列表对象
 * @param index 目标条目下标（越界自动收敛到末尾）
 * @param anim  是否启用滚动动画（LV_ANIM_ON / LV_ANIM_OFF）
 */
void virtual_list_scroll_to_index(lv_obj_t *list, uint32_t index,
                                  lv_anim_enable_t anim);

/** @brief 刷新：按当前滚动位置重新渲染所有可见 cell（数据变更后调用） */
void virtual_list_refresh(lv_obj_t *list);

/**
 * @brief 设置选中条目（-1 表示取消选中）
 *
 * 选中条目会叠加背景高亮（不覆盖绑定回调设置的文字颜色）。
 */
void virtual_list_set_selected(lv_obj_t *list, int32_t index);

/** @brief 获取当前选中条目下标（无选中返回 -1） */
int32_t virtual_list_get_selected(lv_obj_t *list);

/** @brief 设置选中回调（点击某条选中后触发） */
void virtual_list_set_selected_cb(lv_obj_t *list,
                                  virtual_list_selected_cb_t cb);

/**
 * @brief 设置是否允许超长文本跑马灯（默认开启）
 *
 * 开启时超长文本循环滚动（LV_LABEL_LONG_SCROLL_CIRCULAR）；
 * 关闭时改为裁剪（LV_LABEL_LONG_CLIP）。常驻 cell 在滚动时不被重绑
 * （不 set_text / set_pos），因此跑马灯动画不会因滚动而重置，
 * 仅新进入视口的 cell 会从头开始（属正常表现）。
 */
void virtual_list_set_marquee(lv_obj_t *list, bool enable);

/** @brief 获取当前第一个可见条目的下标（调试/定位用） */
uint32_t virtual_list_get_first_index(lv_obj_t *list);

#ifdef __cplusplus
}
#endif
