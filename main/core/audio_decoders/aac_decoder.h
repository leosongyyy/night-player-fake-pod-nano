#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AAC-LC 解码单元（MP4/M4A 容器 + ADTS 裸流）
 *
 * 与 wav_decoder 同构的独立单元：只被 audio_decoder.c 的分发层调用。
 * 载体解析在 mp4_demux.c / adts_parser.c，纯解码在 libhelix-aac 组件。
 *
 * 支持范围：AAC-LC（AOT=2），32/44.1/48kHz，单/双声道；
 * 输出交织 16bit PCM。Helix 每帧解出 1024 帧，而播放任务每次只要 512 帧，
 * 因此本单元内部保留 1024 帧缓冲，分两次吐出（残帧语义见 aac_decoder.c）。
 * HE-AAC/SBR/PS、损坏帧、非 AAC 内容一律打开或读取时报错，绝不输出满幅 PCM。
 */

typedef enum {
    AAC_CONT_M4A = 0,   /* MP4 容器里的 AAC-LC 原始帧 */
    AAC_CONT_ADTS,      /* ADTS 裸流 */
} aac_container_t;

typedef struct aac_decoder aac_decoder_t;

/** @brief 打开；失败返回 NULL 并在 err 写中文原因 */
aac_decoder_t *aac_decoder_open(const char *path, char *err, size_t err_len);

/**
 * @brief 读取交织 16bit PCM
 * @return 实际帧数；0 = 文件结束（残帧已吐完）；-1 = 解码错误
 */
int aac_decoder_read(aac_decoder_t *d, int16_t *out, int max_frames);

/** @brief 跳转到指定秒数（M4A 精确到 sample，ADTS 按帧比例重同步） */
esp_err_t aac_decoder_seek(aac_decoder_t *d, uint32_t seconds);

uint32_t aac_decoder_sample_rate(const aac_decoder_t *d);
uint32_t aac_decoder_channels(const aac_decoder_t *d);
uint64_t aac_decoder_total_frames(const aac_decoder_t *d);

/** @brief 关闭：释放 Helix arena、残帧/原始帧缓冲、载体表并 fclose */
void aac_decoder_close(aac_decoder_t *d);

#ifdef __cplusplus
}
#endif
