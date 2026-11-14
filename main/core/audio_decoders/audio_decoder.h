#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_DEC_UNKNOWN = 0,
    AUDIO_DEC_MP3,
    AUDIO_DEC_FLAC,
    AUDIO_DEC_OGG,
} audio_dec_type_t;

typedef struct audio_decoder audio_decoder_t;

/**
 * @brief 打开音频文件并创建流式解码器（不再整文件读入内存，无 32MB 限制）
 *
 * 支持 .mp3（minimp3，64KB 滑动缓冲）、.flac（dr_flac 文件流）、
 * .ogg（stb_vorbis 文件流，仅 Vorbis）。
 *
 * @param path    文件路径
 * @param err     失败原因输出（可为 NULL）
 * @param err_len 输出缓冲长度
 * @return 解码器；失败返回 NULL
 */
audio_decoder_t *audio_decoder_open(const char *path, char *err, size_t err_len);

/**
 * @brief 读取交织 16bit PCM
 *
 * @param out        输出缓冲（至少 max_frames * channels 个 int16）
 * @param max_frames 最大帧数（建议 >= 2048，MP3 单帧最多 1152）
 * @return 实际帧数；<=0 表示结束或错误
 */
int audio_decoder_read(audio_decoder_t *dec, int16_t *out, int max_frames);

uint32_t audio_decoder_sample_rate(const audio_decoder_t *dec);
uint32_t audio_decoder_channels(const audio_decoder_t *dec);
uint64_t audio_decoder_total_frames(const audio_decoder_t *dec);
const char *audio_decoder_type_name(const audio_decoder_t *dec);

/** 跳转到指定秒数（MP3/FLAC/OGG 均支持；MP3 首次 seek 可能扫描建索引） */
esp_err_t audio_decoder_seek(audio_decoder_t *dec, uint32_t seconds);

void audio_decoder_close(audio_decoder_t *dec);

#ifdef __cplusplus
}
#endif
