#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/hardware/audio/eq.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频播放 API（I2S 主机 + CS43131 DAC + PCM 播放）
 *
 * 硬件连接：
 *  - GPIO40 = BCK（I2S 位时钟，主机输出）
 *  - GPIO38 = LRCK/WS（帧时钟，主机输出）
 *  - GPIO39 = DIN（数据输出）
 *  - MCLK 由板载 24.576MHz 晶振直接提供（不占用 GPIO）
 *
 * 数据格式：I2S 标准（Philips），32bit slot，双声道，
 * 样本为 16bit 左移 16 位（MSB 对齐，与 CS43131 的 32bit slot 匹配）。
 */

/** 采样率枚举（值即 Hz，CS43131 ASP_SPRATE 支持的最低三档） */
typedef enum {
    AUDIO_FS_32000 = 32000,
    AUDIO_FS_44100 = 44100,
    AUDIO_FS_48000 = 48000,
} audio_fs_t;

/**
 * @brief 初始化音频子系统（I2S 通道 + CS43131）
 *
 * 前置条件：touch_init() 已完成（I2C_NUM_0 总线已安装）。
 * 默认采样率 48kHz，默认音量 20 档。
 *
 * @return ESP_OK 成功
 */
esp_err_t audio_init(void);

/**
 * @brief 以指定采样率开始 PCM 播放（配置 CS43131 + I2S 时钟 + 取消静音）
 *
 * 用于向 I2S 写入解码器输出的 PCM 流（见 audio_pcm_write）。
 * 调用前无需额外停止动作；会复用采样率切换逻辑并冲刷 DMA 残留。
 *
 * @param fs 采样率（32000 / 44100 / 48000）
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_start(audio_fs_t fs);

/**
 * @brief 写入交织的 16bit PCM 样本（[L0,R0,L1,R1,...]），阻塞直到送入 I2S DMA
 *
 * 处理链路（FUTURE_PLAN 第 10 点起统一浮点路径，EQ 关闭也走）：
 *   int16 → float（÷32768 归一化）→ [×EQ 预增益 → 最多 10 段 PeakEQ 级联
 *   （eq.h，开启时）] → ×回放增益（audio_set_replay_gain 的 16.16 定点系数
 *   缩放回线性 float）→ Clamp 到 int32 范围 → 左对齐 32bit slot 输出。
 *   ⚠ 浮点尾数 24bit 保留 16bit 内容 + 增益高 8 位小数精度；EQ/增益叠加
 *   超出 int32 的样本被 Clamp（防原整数路径"自然回绕"造成的爆音）。
 *
 * @param samples 交织样本数组
 * @param frames  帧数（每帧 = 左右各 1 个样本）
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_write(const int16_t *samples, size_t frames);

/** 默认回放增益：1.0（0dB），16.16 定点 */
#define AUDIO_REPLAY_GAIN_UNITY 65536u

/**
 * @brief 设置回放增益（16.16 定点线性系数，65536 = 1.0 = 0dB）
 *
 * 切歌时由上层（player_controller）按歌曲元数据的 replaygain 调用，
 * 内部换算为 float 线性系数供 audio_pcm_write 浮点路径使用（EQ 之后、
 * Clamp 之前相乘）。0 或无效值会被钳位到默认 1.0。
 *
 * @param gain_q16 线性增益 × 65536（如 -6dB ≈ 0.5012 → 32854）
 */
void audio_set_replay_gain(uint32_t gain_q16);

/**
 * @brief 停止 PCM 播放（先软斜坡静音，再停 I2S）
 *
 * ⚠ 顺序是"静音 → 等 30ms → 停时钟"：反过来会在 DAC 仍在输出电平时抽掉
 *   BCK/LRCK，耳机里会有可闻的"咔哒"声。
 *
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_stop(void);

/**
 * @brief 优雅关断音频子系统（**关机路径专用**，避免关机爆音）
 *
 * 流程：CS43131 pop-free 掉电序列（软斜坡静音 → 150ms → 关耳放 → 关 ASP
 * → 关晶振 → RESET# 拉低）→ 最后停 I2S 时钟。
 *
 * ⚠ 为什么需要它：S31 关机是 AXP2101 "一刀切"断电，若 DAC/耳放还在工作
 *   状态（带偏置、输出非零），电源轨塌陷会在耳机里炸出一声爆音，关机越快
 *   越明显。
 * ⚠ 必须在**停 I2S 之前**调用（软斜坡静音需要时钟）；本函数内部自己按这个
 *   顺序处理，调用方只需保证没有别的任务还在写 I2S（先 mp3_player_stop 并
 *   等 IDLE）。
 * ⚠ 幂等；audio 未初始化（充电界面拔充电器关机等场景）时为空操作。
 * ⚠ 调用后 DAC 处于复位/掉电态：若随后**没有**真正断电（关机失败），必须
 *   调 audio_recover_after_failed_shutdown() 把音频重新拉起来。
 */
void audio_shutdown(void);

/**
 * @brief 关机失败（PMU 未响应、设备继续运行）后重新拉起音频
 *
 * 重新执行 CS43131 的复位 + 完整配置并恢复原音量；I2S 通道对象仍在，下一次
 * audio_pcm_start() 会重新配置时钟并 enable。未处于关断态时为空操作。
 *
 * @return ESP_OK 成功；其它为 CS43131 重新初始化失败
 */
esp_err_t audio_recover_after_failed_shutdown(void);

/** 音量档位：0 = 静音，1~30 线性映射 20%~80%（人耳有效区间线性化） */
#define AUDIO_VOL_LEVEL_MUTE    0u   /* 静音档：音量寄存器写最小 0xFF */
#define AUDIO_VOL_LEVEL_MIN     1u   /* 最小非静音档（对应 20%） */
#define AUDIO_VOL_LEVEL_MAX     30u  /* 最大档（对应 80%） */
#define AUDIO_VOL_LEVEL_DEFAULT 16u  /* 默认档（NVS 无设置时，对应约 59%） */

/* 档位→百分比映射端点：1 档 = 20%，30 档 = 80%（每档约 2.07 个百分点） */
#define AUDIO_VOL_PERCENT_MIN   35u
#define AUDIO_VOL_PERCENT_MAX   85u

/**
 * @brief 设置音量档位（映射到 CS43131 音量寄存器）
 *
 * 档位 1~30 线性映射 20%~80%；0 档直接写音量寄存器最小值 0xFF 实现静音
 * （原配置下 <10 已几乎无声，无需走 CS43131 静音控制位）。
 *
 * @param level 0~30（0 = 静音，越界自动钳位到 30）
 * @return ESP_OK 成功
 */
esp_err_t audio_set_volume_level(uint8_t level);

/** @brief 当前播放采样率 */
audio_fs_t audio_get_fs(void);

/** @brief 当前音量档位（0~30） */
uint8_t audio_get_volume_level(void);

/* ------------------------------------------------------------------ */
/* 蓝牙输出（2026-11，经典蓝牙 A2DP）                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 音量档位 → 百分比（1 档 = 35%，30 档 = 85%，线性）
 *
 * 供蓝牙绝对音量换算复用，保证 DAC / 蓝牙两条路径的音量曲线一致。
 */
uint8_t audio_volume_level_to_percent(uint8_t level);

/**
 * @brief 进入"蓝牙设备配置优先"模式（连接成功时由 bt_service 调用）
 *
 * 连接期间：音量取该设备记忆的档位、EQ 用该设备的预设（**优先于配置槽位**），
 * 且不写 config_store（避免把设备音量污染到全局配置）。
 *
 * @param volume 设备记忆的音量档位（0~30）
 * @param eq     设备 EQ 预设（NULL 或 band_count=0 = 无 EQ）
 */
void audio_bt_profile_enter(uint8_t volume, const eq_preset_t *eq);

/** @brief 退出设备配置优先模式（断开时调用）：恢复配置槽位的音量与 EQ */
void audio_bt_profile_leave(void);

/** @brief 当前是否处于蓝牙设备配置优先模式 */
bool audio_bt_profile_active(void);

/**
 * @brief 把音频输出切到蓝牙（连接成功 / A2DP 就绪）
 *
 * 动作：静音 DAC、停 I2S、启动蓝牙重采样缓冲、把 EQ 系数切到蓝牙输出
 * 采样率、按"绝对音量/软件音量"重新应用当前音量。正在播放时会自动请求
 * A2DP 起流（由 bt_service 处理）。
 *
 * @param out_fs 蓝牙输出采样率（48000 / 44100）
 */
void audio_bt_output_enable(uint32_t out_fs);

/** @brief 把音频输出切回 DAC（断开 / 关闭蓝牙）：重新配置 I2S + CS43131 */
void audio_bt_output_disable(void);

/** @brief A2DP 协商出的采样率变化（耳机只支持 44.1k 时） */
void audio_bt_set_out_fs(uint32_t out_fs);

/** @brief 绝对音量能力变化后重新应用音量（软件增益 ↔ 绝对音量切换） */
void audio_bt_refresh_volume(void);

/**
 * @brief 耳机侧改了音量（AVRCP 音量变化通知）：同步本机档位但不回发命令
 *
 * @param level 0~30
 */
void audio_volume_from_remote(uint8_t level);

#ifdef __cplusplus
}
#endif
