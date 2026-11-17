#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MP4/M4A 轻量解封装（只处理 AAC-LC 播放必需的 box）
 *
 * 解析路径：moov → trak → mdia → minf → stbl，取 stsd→mp4a→esds 的
 * AudioSpecificConfig（AOT 必须为 2 = AAC-LC），并由 stts/stsz/stco/co64/stsc
 * 建立 sample 索引。mdat 只记录偏移，音频帧按需 fseek+fread，不整文件读入。
 * 所有表与句柄由本模块在 PSRAM 分配，mp4_demux_close() 负责释放；
 * FILE * 由调用方（aac_decoder）打开并关闭。
 */

/** stts 表项：连续 sample_count 个 sample 的时长均为 sample_delta（timescale 单位） */
typedef struct {
    uint32_t sample_count;
    uint32_t sample_delta;
} mp4_stts_entry_t;

/** stsc 表项：从 first_chunk（1 起）开始的 chunk 每块含 samples_per_chunk 个 sample */
typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} mp4_stsc_entry_t;

typedef struct mp4_track {
    FILE *fp;                    /* 借用调用方的 FILE，不负责 fclose */
    uint64_t mdat_offset;        /* mdat 数据区起点 */
    uint64_t mdat_size;          /* mdat 数据区长度 */
    uint32_t timescale;          /* mdhd 的 timescale */
    uint32_t duration_ts;        /* mdhd 的 duration（timescale 单位） */
    uint32_t sample_count;       /* stsz 里的 sample 总数 */
    uint32_t fixed_sample_size;  /* stsz 固定帧长；非 0 时不分配 stsz 表 */
    uint32_t *stsz;              /* 每个 sample 的字节数（PSRAM） */
    uint32_t *stco;              /* chunk 偏移 32bit（PSRAM） */
    uint64_t *co64;              /* chunk 偏移 64bit（PSRAM） */
    bool use_co64;               /* true 表示使用 co64 而非 stco */
    uint32_t chunk_count;
    mp4_stsc_entry_t *stsc;      /* sample→chunk 映射（PSRAM） */
    uint32_t stsc_count;
    mp4_stts_entry_t *stts;      /* 时间→sample 映射（PSRAM） */
    uint32_t stts_count;
    uint8_t *asc;                /* AudioSpecificConfig（PSRAM） */
    size_t asc_len;
    uint32_t cur_sample;         /* 下一个要读的 sample 序号（0 起） */
    uint32_t cur_chunk;          /* 当前 chunk 序号（0 起） */
    uint32_t cur_chunk_start_sample;
    uint32_t cur_chunk_samples;
    uint64_t cur_chunk_offset;   /* 当前 chunk 内下一个 sample 的文件偏移 */
} mp4_track_t;

/**
 * @brief 打开 MP4 文件的第一条 AAC-LC 音轨
 *
 * 非 AAC（objectTypeIndication 非 0x40/0x66/0x67）、非 LC（AOT≠2）、
 * 采样率不在 32/44.1/48k、声道不是 1/2、表长度异常都返回明确错误。
 *
 * @param fp      已以 "rb" 打开的 FILE（调用方拥有）
 * @param out     成功时返回 PSRAM 中的轨道句柄
 * @param err     失败原因（中文，可 NULL）
 * @param err_len err 缓冲长度
 */
esp_err_t mp4_demux_open(FILE *fp, mp4_track_t **out, char *err, size_t err_len);

/**
 * @brief 读取下一个 AAC 原始帧（不含 ADTS 头）
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 已到 sample 末尾；其它为损坏帧错误
 */
esp_err_t mp4_demux_read_frame(mp4_track_t *t, uint8_t *buf, size_t cap, size_t *len);

/** @brief 跳转到指定 sample（0 起），后续 read_frame 从该 sample 开始 */
esp_err_t mp4_demux_seek_sample(mp4_track_t *t, uint32_t sample_idx);

/** @brief 由秒数换算 sample 序号（用 stts 精确累加，失败回退比例估算） */
esp_err_t mp4_demux_sample_at_seconds(const mp4_track_t *t, uint32_t seconds,
                                      uint32_t *sample_idx);

/** @brief 返回 ASC 里的采样率/声道/AOT（AOT 恒为 2，打开时已校验） */
esp_err_t mp4_demux_audio_config(const mp4_track_t *t, uint32_t *sample_rate,
                                 uint8_t *channels, uint8_t *aot);

/**
 * @brief 提取 moov/udta/meta/ilst/covr/data 内嵌封面（PSRAM 分配）
 *
 * 返回的是字节流，调用方须自行做 JPEG 魔数校验；PNG 等同理会被上层回退。
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 无封面；其它为解析/内存错误
 */
esp_err_t mp4_demux_find_cover(FILE *fp, uint8_t **jpeg, uint32_t *len);

/** @brief 释放轨道及全部 PSRAM 表；不关闭 fp；t 可为 NULL */
void mp4_demux_close(mp4_track_t *t);

#ifdef __cplusplus
}
#endif
