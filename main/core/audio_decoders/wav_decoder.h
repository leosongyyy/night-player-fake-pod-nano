#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WAV（RIFF/WAVE）流式解码器
 *
 * 本工程唯一"自己写"的格式实现（MP3/FLAC/OGG 都是第三方库），因此单独成单元，
 * 只被 core/audio_decoders/audio_decoder.c 的分发层调用。
 *
 * 支持范围：
 *   · 容器：RIFF/WAVE（RF64 也接受，data 长度缺省时按文件长度兜底）；
 *   · 编码：PCM（fmt tag 1，8/16/24/32bit）+ IEEE float（tag 3，32bit），
 *           以及 WAVE_FORMAT_EXTENSIBLE（0xFFFE）里的 PCM/float 子格式
 *           （常见于 24bit-in-32 容器、带 channel mask 的文件）；
 *   · 声道：单声道、双声道（>2 直接拒绝，播放链路只认 1/2）；
 *   · 采样率：8k~192k 读出来原样上报，32k/44.1k/48k 之外由播放任务按
 *     "格式不支持"跳过（与 MP3/FLAC/OGG 一致）。
 * 不支持：ADPCM / A-law / mu-law 等压缩 WAV（打开时直接报错，避免播成噪声）、
 *         64bit float、RIFX（大端 RIFF）。
 *
 * 输出统一为**交织 16bit PCM**（单声道就是 1 个样本/帧），整数采样取容器高
 * 16bit（MSB 对齐，等价于按位宽归一化），float 裁剪到 [-1,1] 后乘 32767。
 * 数据按帧 fread，49MB 的 WAV 也只占一个 512 帧 × block_align 的缓冲。
 */

typedef struct wav_decoder wav_decoder_t;

/**
 * @brief 打开 WAV 文件并解析 fmt/data
 *
 * @param path    文件路径
 * @param err     失败原因输出（可为 NULL）
 * @param err_len 输出缓冲长度
 * @return 解码器；失败返回 NULL（err 里有中文原因）
 */
wav_decoder_t *wav_decoder_open(const char *path, char *err, size_t err_len);

/**
 * @brief 读取交织 16bit PCM
 *
 * @param out        输出缓冲（至少 max_frames * channels 个 int16）
 * @param max_frames 最大帧数
 * @return 实际帧数；0 表示文件结束；-1 表示错误
 */
int wav_decoder_read(wav_decoder_t *dec, int16_t *out, int max_frames);

/** @brief 跳转到指定秒数（定长帧直接算文件偏移，不需要索引） */
esp_err_t wav_decoder_seek(wav_decoder_t *dec, uint32_t seconds);

uint32_t wav_decoder_sample_rate(const wav_decoder_t *dec);
uint32_t wav_decoder_channels(const wav_decoder_t *dec);
uint64_t wav_decoder_total_frames(const wav_decoder_t *dec);

/** @brief 关闭并释放（安全；dec 可为 NULL） */
void wav_decoder_close(wav_decoder_t *dec);

#ifdef __cplusplus
}
#endif
