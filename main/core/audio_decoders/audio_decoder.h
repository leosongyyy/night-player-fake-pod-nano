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
    AUDIO_DEC_WAV,
} audio_dec_type_t;

typedef struct audio_decoder audio_decoder_t;

/**
 * @brief 打开音频文件并创建流式解码器（不整文件读入内存）
 *
 * 支持 .mp3（minimp3，128KB IO 缓冲）、.flac（dr_flac 文件流）、
 * .ogg（Tremor/libvorbisidec 定点解码，文件流，仅 Vorbis）、
 * .wav（RIFF 容器内的线性 PCM / IEEE float，见下方说明）。
 *
 * WAV 支持范围（2026-11 加入）：RIFF/WAVE 的 PCM（fmt=1，8/16/24/32bit）与
 * IEEE float（fmt=3，32bit），含 WAVE_FORMAT_EXTENSIBLE（0xFFFE）的 PCM/float
 * 子格式；单声道/双声道；块大小由 fmt 的 block_align 决定，按帧流式读取，
 * **不整文件读入内存**（49MB 的 WAV 也只占几 KB 缓冲）。ADPCM / mu-law /
 * A-law 等压缩 WAV 不支持，打开时直接报错（避免播成噪声）。
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
 * @param max_frames 最大帧数（建议 >= 512；更小请求由解码器内部残帧缓冲承接）
 * @return 实际帧数；<=0 表示结束或错误
 */
int audio_decoder_read(audio_decoder_t *dec, int16_t *out, int max_frames);

uint32_t audio_decoder_sample_rate(const audio_decoder_t *dec);
uint32_t audio_decoder_channels(const audio_decoder_t *dec);
uint64_t audio_decoder_total_frames(const audio_decoder_t *dec);
const char *audio_decoder_type_name(const audio_decoder_t *dec);

/** 跳转到指定秒数（MP3/FLAC/OGG/WAV 均支持；MP3 首次 seek 可能扫描建索引） */
esp_err_t audio_decoder_seek(audio_decoder_t *dec, uint32_t seconds);

void audio_decoder_close(audio_decoder_t *dec);

#ifdef __cplusplus
}
#endif
