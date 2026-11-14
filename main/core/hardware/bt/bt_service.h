#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/storage/bt_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file bt_service.h
 * @brief 经典蓝牙音频服务（A2DP Source / SBC + AVRCP CT 绝对音量）
 *
 * 职责：
 *   - 蓝牙控制器 + Bluedroid 的启动/关闭（**只做 Classic BR/EDR**）；
 *   - GAP：设备名、扫描（inquiry）、配对（SSP 确认 / legacy PIN）、bond 管理；
 *   - A2DP Source：连接保存过的设备、协商 SBC 参数（48k 优先、44.1k 兜底）、
 *     媒体通道起流/挂起（播放开始/暂停时由 audio 层请求）；
 *   - AVRCP CT：探测耳机是否支持绝对音量；支持则把 0~30 档映射成 0~127
 *     发给耳机（软件增益保持 1.0），不支持则退回软件音量；耳机侧调音量时
 *     同步回放音量档位并写回该设备的配置；
 *   - **设备配置优先级**：连接时把该设备保存的音量/EQ 应用到音频链，
 *     断开时恢复配置槽位的音量/EQ（见 audio_bt_profile_*）。
 *
 * 线程模型：所有协议栈操作都投递到本模块的 `bt_svc` 任务（内部 RAM 栈，
 * Core1）串行执行；Bluedroid 回调只做轻量状态更新/投递，不在回调里做
 * 重活（重采样器分配、EQ 重算、CS43131 I2C 等）。
 *
 * UI 侧不注册回调，**轮询**本模块的状态（bt_settings 的 500ms 定时器），
 * 与项目里 player_ticker / settings 页的轮询风格一致，避免跨任务碰 LVGL。
 */

/** 蓝牙电源状态 */
typedef enum {
    BT_SVC_POWER_OFF = 0,   /* 栈未启动（关闭） */
    BT_SVC_POWER_STARTING,  /* 正在初始化控制器/协议栈 */
    BT_SVC_POWER_ON,        /* 栈就绪（可扫描/连接） */
    BT_SVC_POWER_STOPPING,  /* 正在关闭 */
} bt_svc_power_t;

/** 扫描到的一个设备（UI 列表用） */
typedef struct {
    uint8_t bda[6];
    char    name[BT_STORE_NAME_MAX];
    int     rssi;           /* dBm；未知为 -128 */
} bt_scan_result_t;

/** 初始化（创建命令队列 + bt_svc 任务）；按 NVS 记忆的开关状态决定是否启动 */
void bt_service_init(void);

/* ---------------- 开关 / 状态 ---------------- */

/** @brief 用户的蓝牙开关意图（true = 应开启；与实际栈状态可能短暂不一致） */
bool bt_service_is_enabled(void);

/** @brief 打开/关闭蓝牙（会写 NVS 记忆；异步执行，立即返回） */
void bt_service_set_enabled(bool on);

/** @brief 实际电源状态（UI 轮询） */
bt_svc_power_t bt_service_power_state(void);

/** @brief A2DP 是否已连接 */
bool bt_service_is_connected(void);

/** @brief A2DP 媒体流是否在发送中 */
bool bt_service_is_streaming(void);

/** @brief 当前连接的设备在 bt_store 中的下标；未连接返回 -1 */
int bt_service_connected_index(void);

/** @brief 当前连接设备名（未连接返回 ""） */
const char *bt_service_connected_name(void);

/** @brief 协商出的 SBC 输出采样率（48000/44100；未连接返回 0） */
uint32_t bt_service_output_fs(void);

/** @brief 上次连接是否因耳机不支持 SBC 48k/44.1k 而失败（UI 可 Toast 提示） */
bool bt_service_codec_unsupported(void);

/* ---------------- 连接 / 设备管理 ---------------- */

/**
 * @brief 连接 bt_store 中下标为 idx 的设备
 *
 * 若当前已连接其它设备：**先断开当前设备**，等 A2DP 链路真正收尾后再连
 * 目标设备（服务层异步处理，期间 `bt_service_is_connected()` 会短暂为
 * false）。目标连接失败则保持断开，**不会回退到前一个设备**。
 */
void bt_service_connect_index(int idx);

/** @brief 断开当前连接 */
void bt_service_disconnect(void);

/** @brief 删除设备：断开（若已连接）→ 移除系统 bond → 从 bt_store 删除 */
void bt_service_forget(int idx);

/* ---------------- 扫描 / 配对 ---------------- */

/** @brief 开始经典蓝牙扫描（约 12.8 秒；扫描期间才可被发现） */
void bt_service_scan_start(void);

/** @brief 停止扫描 */
void bt_service_scan_stop(void);

/** @brief 是否正在扫描 */
bool bt_service_scan_active(void);

/** @brief 当前扫描结果数量（最多 16 个） */
int bt_service_scan_count(void);

/** @brief 读取第 i 个扫描结果 */
bool bt_service_scan_get(int i, bt_scan_result_t *out);

/** @brief 连接第 i 个扫描结果：设备表未满则保存，然后发起配对/连接 */
void bt_service_scan_connect(int i);

/* ---------------- 音量 / EQ 联动 ---------------- */

/**
 * @brief 音量变化后通知：连接期间写回该设备的音量记忆（RAM，延迟落盘）
 *
 * 由按键/控制中心/设置页在 audio_set_volume_level 之后调用；未连接时无操作
 * （全局音量由 config_store 自己保存）。
 */
void bt_service_note_volume(uint8_t level);

/** @brief 设备 EQ 变更后即时生效（仅当 idx 是当前连接设备时 eq_apply） */
void bt_service_apply_device_eq(int idx);

/** @brief 当前连接设备是否开启了安静模式（睡眠定时到点用） */
bool bt_service_connected_quiet_mode(void);

#ifdef __cplusplus
}
#endif
