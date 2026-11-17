#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ADTS 裸流（.aac）轻量解析器
 *
 * 打开时全文件轻量帧计数（只读 7 字节头 + 跳帧体）得到精确帧数与时长；
 * 逐帧读整帧（含 7/9 字节 ADTS 头）交给 Helix 解码。
 * 只接受 profile=1（AAC-LC / AOT=2）、32/44.1/48kHz、单/双声道。
 * HE-AAC 常以上报核心采样率 22.05kHz 的形式出现，会被采样率白名单拒绝。
 */

typedef struct {
    FILE *fp;                 /* 借用调用方的 FILE，不负责 fclose */
    uint64_t file_size;
    uint64_t first_frame_offset;
    uint64_t cur_offset;      /* 下一帧起点 */
    uint32_t frames_total;    /* 打开时数出的帧数 */
    uint32_t cur_frame;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t profile;          /* Helix 的 profile：1 = AAC_LC */
    uint8_t aot;              /* = 2 */
} adts_state_t;

/** @brief 校验首帧参数、统计总帧数；失败时写中文原因 */
esp_err_t adts_open(FILE *fp, adts_state_t *st, char *err, size_t err_len);

/** @brief 读取下一整帧（含 ADTS 头）；ESP_ERR_NOT_FOUND 表示文件结束 */
esp_err_t adts_read_frame(adts_state_t *st, uint8_t *buf, size_t cap, size_t *len);

/** @brief 按帧号比例粗定位 + 向后有限次重同步（误差 ≤ 1 帧） */
esp_err_t adts_seek_seconds(adts_state_t *st, uint32_t seconds);

/** @brief 总帧数（每帧 1024 PCM 帧） */
uint64_t adts_total_frames(const adts_state_t *st);

/** @brief 只重置游标状态，不关闭 fp */
void adts_close(adts_state_t *st);

#ifdef __cplusplus
}
#endif
