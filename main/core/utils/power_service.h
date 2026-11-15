#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file power_service.h
 * @brief 电源服务（息屏/亮屏管理 + 无操作自动息屏）
 *
 * 本模块汇聚"电源相关状态机与后台守护"：
 * - 息屏 = 屏幕全黑（亮度 0）+ 挂起 LVGL（停止刷屏、触摸轮询与 LVGL 时间）
 *          + CPU 固定降到 80MHz（esp_pm_configure 强制 max=min=80）。
 *   亮屏 = CPU 恢复 320MHz + 恢复 LVGL + 恢复用户配置亮度。
 *   ⚠ 不用 DFS 动态调频（实测释放频率锁后空闲期降频不明显），改为直接固定
 *   频率；息屏统一 80MHz，播放/暂停不再区分频点。
 * - 无操作（按键/触摸）超时自动息屏（config_store 的 screen_off_s，默认 30s）
 * - **息屏且不在播放（暂停 / 停止）持续 `CONFIG_PAUSE_AUTO_SHUTDOWN_MIN`
 *   分钟（默认 10）→ AXP2101 PMU 真关机**（2026-11 由 S3 的"暂停超时伪关机"
 *   恢复而来；充电中不触发，避免插着电自动关机）
 * - 低电量持续 60 秒（<5% 且未充电）→ AXP2101 真关机
 *
 * ⚠ S31 的关机一律是 AXP2101 **真断电**（`fake_shutdown` 写 `0x10[0]=1`），
 *   不是 S3 的 deepsleep 伪关机；关机后按电源键 / 插充电器开机。
 *
 * 入口：
 * - 手动切换息屏/亮屏：双键同时按下（BUTTON_EVENT_BOTH），见 ui/key_handler
 * - 自动息屏：由本模块后台任务（power_svc，Core1 每秒醒来一次）检测
 *
 * 息屏期间的行为（见 ui/key_handler）：
 * - A 键短按 = 播放/暂停；B 键短按 = 下一曲；B 键长按 = 上一曲；
 * - A 键长按 = 先亮屏再关机（显示「正在关机...」，保存后由 PMU 断电）；
 * - 双键同时按下 = 亮屏。
 *
 * 依赖：lvgl_port（挂起/恢复）、display（亮度）、esp_pm（固定频率）、
 *       config_store（超时配置）、battery（电池健康保护 tick）、
 *       fake_shutdown（关机，S31 = AXP2101 软关机）。
 *       需在 lvgl_port_init 之后调用 power_service_init。
 */

/**
 * @brief 初始化电源服务：固定 CPU 频率 320MHz、订阅触摸活动、启动
 *        后台守护任务（无操作超时自动息屏）
 *
 * CONFIG_PM_ENABLE 未开启时降级：不调频，其余功能（全黑 + 挂起 LVGL）仍正常。
 * 可重复调用（幂等）。
 */
void power_service_init(void);

/** @brief 息屏（全黑 + 挂起 LVGL + CPU 固定 80MHz）。已息屏时直接返回。
 *         线程安全：内部持状态互斥锁，可从按键任务/守护任务调用。 */
void power_service_screen_off(void);

/** @brief 亮屏（恢复亮度 + 恢复 LVGL + CPU 恢复 320MHz）。未息屏时直接返回。
 *         线程安全。亮屏后重置"最近活动时间"，避免立刻再次自动息屏。 */
void power_service_screen_on(void);

/** @brief 息屏/亮屏切换（双键同时按下调用） */
void power_service_screen_toggle(void);

/** @brief 当前是否处于息屏状态 */
bool power_service_is_screen_off(void);

/**
 * @brief 记录一次用户活动（按键/触摸），刷新自动息屏的计时基准
 *
 * 任意线程可调用（内部只写一个 int64 时间戳）。息屏状态下调用也会刷新
 * 计时，但息屏时守护任务跳过息屏检查，无副作用；亮屏时会自动重置。
 */
void power_service_note_activity(void);

/**
 * @brief 距最近一次用户活动的空闲秒数（无操作已持续多久）
 *
 * 供 UI 复用"空闲计时"实现挂机行为（如正在播放页挂机 10 秒自动隐藏
 * 标题栏/控制栏）。任意线程可调用。
 */
uint32_t power_service_get_idle_sec(void);

/**
 * @brief 暂时禁止"无操作自动息屏"（按住唤醒），用于网络对时等长流程
 *
 * true = 保持亮屏（内部同时刷新活动时间，避免刚置位就超时），直到调用方
 * 置回 false。仅影响自动息屏，手动息屏/双键息屏不受影响。
 *
 * ⚠ 这是"抑制"而不是"重置计时"：网络对时最长可达一分多钟，期间没有
 *   触摸活动，单靠 note_activity() 只能撑过一个超时周期。
 */
void power_service_set_hold_awake(bool hold);

/**
 * @brief 播放线程的"性能需求"（2026-11，蓝牙音频用）
 *
 * 熄屏时默认把 CPU 固定到 80MHz 省电；但蓝牙 A2DP 输出要做**重采样 + EQ +
 * SBC 编码**，80MHz 实测会卡顿/掉音。播放线程（bt_audio）在流开始时声明
 * POWER_PERF_BT_AUDIO，熄屏频率改为 **160MHz**；流挂起/停止时撤销。
 *
 * 说明：这是"多路需求取并集"的简化实现——任何一个请求处于 on 状态就提高
 * 熄屏频率；亮屏始终 320MHz，不受影响。任意任务可调用，内部线程安全。
 */
typedef enum {
    POWER_PERF_BT_AUDIO = 0,   /* 蓝牙音频（重采样 + SBC 编码） */
    POWER_PERF_COUNT
} power_perf_req_t;

/**
 * @brief 置位/清除一个性能需求
 *
 * @param req 需求类别（见 power_perf_req_t）
 * @param on  true = 需要更高性能；false = 撤销
 */
void power_service_perf_set(power_perf_req_t req, bool on);

/**
 * @brief 进入/退出"安静模式"（蓝牙不断电的伪关机）
 *
 * 进入后：CPU 固定 80MHz、不再执行低电自动关机与自动息屏检查（低电/超时
 * 由 quiet_mode 自己的监控任务按 15%/12h 规则处理），避免与 fake_shutdown
 * 的亮屏提示流程冲突。
 */
void power_service_set_quiet_mode(bool on);

#ifdef __cplusplus
}
#endif
