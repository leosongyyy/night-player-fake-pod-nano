#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 深调用回调：在 deep_worker 大栈任务上下文中执行 */
typedef void (*deep_worker_fn_t)(void *arg);

/**
 * @brief 初始化深路径执行器（常驻大栈任务，固定 Core0）
 *
 * 创建"deep_worker"任务（栈 8KB、优先级 6）+ 请求/完成信号量与投递互斥锁。
 * 必须在任何 deep_worker_call() 之前调用（app_main 早期）。
 *
 * 背景：息屏/亮屏（esp_pm_configure + AMOLED 重新初始化 + lv_refr_now
 * 全屏重刷）与伪关机（停外设 + LVGL 提示渲染 + deepsleep）的调用链很深，
 * 历史上导致 button/power_service/fake_shutdown 等任务各自加大栈（8KB），
 * 内部 RAM 被多份重复大栈吃紧。本模块把这些深链收敛到唯一一个常驻大栈
 * 任务：业务任务调用 deep_worker_call() 只做"投递 + 等待完成"（浅调用），
 * 自身栈可按需收紧。
 */
void deep_worker_init(void);

/**
 * @brief 投递一个深调用并等待其完成（同步）
 *
 * fn 在 deep_worker 任务上下文执行（栈 8KB），返回时 fn 已执行完毕。
 * 多个调用方可安全并发调用：内部互斥锁保证请求串行排队，一次只执行一个。
 *
 * ⚠ 约定：
 * - fn 内禁止再调用 deep_worker_call()（会持锁等待自身，死锁）；
 * - 调用方不要在持有 lvgl_port_lock() 时调用本函数——fn 内部通常会
 *   自行获取 LVGL 锁，调用方持锁等待会与 fn 互等（死锁）。
 *
 * @param fn  深调用回调（非空）
 * @param arg 透传给 fn 的参数
 * @return true 执行完成；false 未初始化/参数非法/超时（5s，executor 卡死兜底）
 */
bool deep_worker_call(deep_worker_fn_t fn, void *arg);

#ifdef __cplusplus
}
#endif
