#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频播放 API（I2S 主机 + CS43131 DAC + 正弦波生成）
 *
 * 硬件连接：
 *  - GPIO40 = BCK（I2S 位时钟，主机输出）
 *  - GPIO38 = LRCK/WS（帧时钟，主机输出）
 *  - GPIO39 = DIN（数据输出）
 *  - MCLK 由板载 24.576MHz 晶振直接提供（不占用 GPIO）
 *
 * 数据格式：I2S 标准（Philips），32bit slot，双声道，
 * 样本为 16bit 正弦值左移 16 位（MSB 对齐，与 CS43131 的 32bit slot 匹配）。
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
 * 默认采样率 48kHz，默认音量 80%。
 *
 * @return ESP_OK 成功
 */
esp_err_t audio_init(void);

/**
 * @brief 播放指定采样率/频率的正弦波（双声道同相）
 *
 * 若正在播放会先停止；内部完成 CS43131 静音 → 采样率切换（仅写寄存器，
 * 不切 MCLK）→ I2S 时钟重配 → 取消静音 → 启动播放任务。
 *
 * @param fs       采样率（44100 或 48000）
 * @param freq_hz  正弦波频率（Hz），如 500 / 1000
 * @return ESP_OK 成功
 */
esp_err_t audio_play_tone(audio_fs_t fs, uint16_t freq_hz);

/**
 * @brief 停止播放（停止任务并静音 DAC）
 * @return ESP_OK 成功
 */
esp_err_t audio_stop(void);

/**
 * @brief 以指定采样率开始 PCM 播放（配置 CS43131 + I2S 时钟 + 取消静音）
 *
 * 用于向 I2S 写入解码器输出的 PCM 流（见 audio_pcm_write）。
 * 调用前无需先 audio_stop；会复用采样率切换逻辑并冲刷 DMA 残留。
 *
 * @param fs 采样率（32000 / 44100 / 48000）
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_start(audio_fs_t fs);

/**
 * @brief 写入交织的 16bit PCM 样本（[L0,R0,L1,R1,...]），阻塞直到送入 I2S DMA
 *
 * @param samples 交织样本数组
 * @param frames  帧数（每帧 = 左右各 1 个样本）
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_write(const int16_t *samples, size_t frames);

/**
 * @brief 停止 PCM 播放（停 I2S + 静音 DAC）
 * @return ESP_OK 成功
 */
esp_err_t audio_pcm_stop(void);

/**
 * @brief 设置音量（映射到 CS43131 音量寄存器）
 * @param percent 0~100（100 = 0dB 最大音量）
 * @return ESP_OK 成功
 */
esp_err_t audio_set_volume(uint8_t percent);

/** @brief 是否正在播放 */
bool audio_is_playing(void);

/** @brief 当前播放采样率 */
audio_fs_t audio_get_fs(void);

/** @brief 当前播放频率（Hz） */
uint16_t audio_get_freq(void);

/** @brief 当前音量（0~100） */
uint8_t audio_get_volume(void);

#ifdef __cplusplus
}
#endif
