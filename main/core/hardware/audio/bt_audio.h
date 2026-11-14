#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file bt_audio.h
 * @brief 蓝牙 A2DP 输出链路（PCM 缓冲 + 重采样 + EQ + SBC 喂数据）
 *
 * 处理链（**按需求：EQ 放在重采样之后**）：
 *
 *   MP3 解码 int16（源采样率 fs_src，32k/44.1k/48k）
 *     → 归一化 float
 *     → **重采样到蓝牙输出采样率 fs_out**（A2DP 协商 48k 优先、44.1k 兜底）
 *     → **EQ**（eq.h，系数按 fs_out 计算）
 *     → ×回放增益 ×软件音量（仅耳机不支持绝对音量时）
 *     → Clamp 成 int16
 *     → PSRAM ringbuf（约 340ms 抖动缓冲）
 *     → A2DP source 数据回调（协议栈内部 SBC 编码器从这里拉 PCM）
 *
 * 为什么 EQ 放在重采样之后：
 *   - EQ 的 biquad 系数与采样率强相关（eq_notify_sample_rate），把 EQ 放在
 *     输出端只需维护一套"以 fs_out 为基准"的系数；源采样率变化（44.1k/48k
 *     切歌）不会让已生效的 EQ 偏频；
 *   - 重采样是线性时不变操作，先 EQ 后重采样与先重采样后 EQ 在数学上等价
 *     （忽略数值精度），放在后面还能少一次全采样率处理。
 *
 * 与 audio.c 的分工：
 *   - audio.c 负责"输出路由"（DAC / BT 二选一）、音量语义、绝对音量；
 *   - 本模块只负责 BT 这一侧的 DSP/缓冲，以及"播放线程的性能需求"通知
 *     （流开始时告诉 power_service 熄屏也别降到 80MHz）。
 *
 * 线程模型：
 *   - `bt_audio_write` 只由 mp3_out 任务（Core1）调用；
 *   - `bt_audio_data_cb` 由 Bluedroid A2DP source 任务调用（只做 memcpy）；
 *   - 激活/去激活/采样率变化由 bt_service 任务或 audio 调用点触发，
 *     都发生在数据流静止期（激活时如果正在播放，会由 audio 重新起流）。
 */

/** 抖动缓冲大小（字节，int16 交织立体声）：96KB ≈ 48k 下 0.5s */
#define BT_AUDIO_RING_BYTES   (96 * 1024)

/** 单次处理的最大帧数（= audio.c 的块大小，512 帧 @48k ≈ 10.7ms） */
#define BT_AUDIO_MAX_FRAMES   512

/** 绝对音量回调：耳机支持 AVRCP 绝对音量时，audio 把档位发到这里 */
typedef void (*bt_audio_absvol_cb_t)(uint8_t level);

/** 流控制回调：播放开始/暂停时请求 bt_service 起流/挂起 */
typedef void (*bt_audio_stream_cb_t)(bool start);

/**
 * @brief 初始化（创建 ringbuf + 分配 DSP 临时缓冲；不启动蓝牙）
 *
 * 可在 audio_init 之后、bt_service_init 之前调用。
 */
esp_err_t bt_audio_init(void);

/* ---------------- 输出激活（由 audio.c 调用） ---------------- */

/**
 * @brief 激活蓝牙输出：设定源/输出采样率并清空缓冲
 *
 * @param src_fs 当前播放源采样率（Hz）
 * @param out_fs 蓝牙输出采样率（48000 / 44100）
 */
void bt_audio_output_start(uint32_t src_fs, uint32_t out_fs);

/** @brief 去激活蓝牙输出（断开/关机）：停止接收 PCM 并清空缓冲 */
void bt_audio_output_stop(void);

/** @brief 蓝牙输出当前是否激活（audio.c 的路由判据） */
bool bt_audio_is_active(void);

/**
 * @brief 播放源采样率变化（切歌 / audio_pcm_start）
 *
 * 若与当前不同则重建重采样器；输出采样率不变时 EQ 系数不受影响。
 */
void bt_audio_set_source_fs(uint32_t fs);

/** @brief 蓝牙输出采样率变化（A2DP 协商结果），会重建重采样器并通知 EQ */
void bt_audio_set_out_fs(uint32_t fs);

/** @brief 当前蓝牙输出采样率（未激活/未协商时返回 0） */
uint32_t bt_audio_get_out_fs(void);

/* ---------------- 数据通路 ---------------- */

/**
 * @brief 写入一帧交织 int16 PCM（源采样率）
 *
 * 内部完成重采样 → EQ → 增益 → int16 → ringbuf；ringbuf 满时最多等 20ms
 * （背压节奏），超时丢弃本块（避免卡死播放任务）。
 */
void bt_audio_write(const int16_t *samples, size_t frames);

/** @brief 清空 ringbuf（seek / 暂停 / 切歌） */
void bt_audio_flush(void);

/** @brief 设置回放增益（16.16 定点线性系数，65536 = 0dB；EQ 之后相乘） */
void bt_audio_set_gain_q16(uint32_t gain_q16);

/** @brief 设置软件音量线性系数（仅耳机不支持绝对音量时非 1.0） */
void bt_audio_set_soft_gain(float gain);

/* ---------------- 绝对音量 ---------------- */

/** @brief 注册绝对音量发送回调（bt_service 注册） */
void bt_audio_set_absvol_handler(bt_audio_absvol_cb_t cb);

/** @brief 告知耳机是否支持绝对音量（AVRCP 能力探测结果） */
void bt_audio_set_absvol_supported(bool supported);

/** @brief 耳机是否支持绝对音量 */
bool bt_audio_absvol_supported(void);

/** @brief 把音量档位作为绝对音量发给耳机（仅 supported 时有效） */
void bt_audio_send_absvol(uint8_t level);

/* ---------------- 流控制 / 性能需求 ---------------- */

/** @brief 注册"起流/挂起"回调（bt_service 注册；A2DP 媒体通道控制） */
void bt_audio_set_stream_handler(bt_audio_stream_cb_t cb);

/** @brief 请求起流/挂起（audio.c 在 audio_pcm_start/stop 时调用） */
void bt_audio_request_stream(bool start);

/**
 * @brief A2DP 流状态变化通知（bt_service 在 AUDIO_STATE_EVT 调用）
 *
 * streaming=true 时向 power_service 声明"播放线程需要更高性能"：
 * 熄屏时 CPU 频率从 80MHz 提到 160MHz（重采样 + SBC 编码需要余量）。
 */
void bt_audio_notify_streaming(bool streaming);

/**
 * @brief A2DP Source 数据回调（bt_service 在栈启动时注册给协议栈）
 *
 * ⚠ 内部使用：由 Bluedroid A2DP source 任务调用，从 ringbuf 拷贝已编码前的
 *   PCM（不足补 0）。返回值必须等于 len（协议栈约定）。
 */
int32_t bt_audio_data_cb(uint8_t *buf, int32_t len);

#ifdef __cplusplus
}
#endif
