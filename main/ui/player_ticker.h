#pragma once

#include "core/player/player_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file player_ticker.h
 * @brief 播放器状态共享轮询 timer（UI 层订阅复用）
 *
 * 统一一个周期 LVGL timer 拉取 player_controller 状态快照并广播给所有
 * 订阅者，避免每个 UI 页面各自创建轮询 timer。订阅者自己决定刷新的粒度：
 *   - 连续变化数据（进度/时长）：每 tick 直接刷新；
 *   - 离散变化数据（曲目/标题/高亮）：对比自己暂存的旧值，变了才刷新。
 *
 * ⚠ 订阅回调在 LVGL timer 上下文中执行（天然持 LVGL 锁），可直接操作
 *   UI 对象；回调中拿到的状态指针仅当次有效，勿保存复用。
 */

/** 订阅回调：每 tick 收到播放器状态快照指针 */
typedef void (*player_ticker_cb_t)(const player_controller_status_t *st,
                                   void *ctx);

/**
 * @brief 订阅周期刷新（内部懒创建共享 timer，所有订阅者共用同一个）
 *
 * @param cb  回调（不可为空）
 * @param ctx 回调上下文（原样传回，可为 NULL）
 */
void player_ticker_subscribe(player_ticker_cb_t cb, void *ctx);

/**
 * @brief 取消订阅（cb 与 ctx 需与订阅时一致，否则不生效）
 */
void player_ticker_unsubscribe(player_ticker_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
