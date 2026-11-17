/**
 * ADTS 裸流（.aac）解析 —— 同步字/帧长校验、全文件帧计数、比例 seek
 *
 * 坏曲防护：头校验失败、帧长越界、参数中途变化一律返回明确错误，
 * 绝不把任意字节当音频送进解码器（防止输出满幅噪声）。
 */

#include "core/audio_decoders/adts_parser.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* ESP-IDF 6.1 移除了 ESP_ERR_INVALID_DATA（原 0x105），此处映射到语义最接近的
 * ESP_ERR_INVALID_SIZE（数据内容/尺寸 invalid）。保留坏曲拒绝的明确错误语义，
 * 避免因 IDF 版本差异导致编译失败。若后续 IDF 恢复该宏，可用 #ifndef 自动让位。 */
#ifndef ESP_ERR_INVALID_DATA
#define ESP_ERR_INVALID_DATA ESP_ERR_INVALID_SIZE
#endif

static const char *TAG = "adts";

/* ADTS 帧长是 13bit，最大 8191 字节（含头） */
#define ADTS_FRAME_MAX       8191u
#define ADTS_HEADER_BYTES    7u
/* 打开时找首个同步字最多扫描 64KB，避免对非 ADTS 文件全盘扫 */
#define ADTS_SYNC_SCAN_MAX   (64u * 1024u)
/* seek 重同步：最多试 64 个候选帧，最多扫 64KB（两个魔法数字都需注释） */
#define ADTS_RESYNC_MAX_FRAMES 64u
#define ADTS_RESYNC_SCAN_BYTES (64u * 1024u)
/* 帧数上限，防止伪造文件把计数循环拖死 */
#define ADTS_MAX_FRAMES      1000000u
/* Helix 每帧输出的 PCM 帧数 */
#define ADTS_SAMPLES_PER_FRAME 1024u

/* ADTS sampling_frequency_index → Hz（只用到 3/4/5） */
static const uint32_t adts_samp_rate_tab[12] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000,
};

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t profile;      /* ADTS 的 2bit profile：1 = LC */
    uint32_t frame_length;
    uint8_t header_bytes; /* 7（无 CRC）或 9（有 CRC） */
    uint8_t num_blocks;   /* 一个 ADTS 帧内的 raw_data_block 数 */
} adts_header_t;

static void adts_set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* 解析 7 字节 ADTS 固定头；多的一字节 CRC 不参与校验 */
static esp_err_t adts_parse_header(const uint8_t *h, adts_header_t *out,
                                   char *err, size_t err_len)
{
    if (h[0] != 0xFF || (h[1] & 0xF0u) != 0xF0u) {
        adts_set_err(err, err_len, "ADTS 同步字无效");
        return ESP_ERR_INVALID_DATA;
    }
    if (((h[1] >> 1) & 0x03u) != 0) {
        adts_set_err(err, err_len, "ADTS layer 不是 0");
        return ESP_ERR_INVALID_DATA;
    }
    uint8_t profile = (uint8_t)((h[2] >> 6) & 0x03u);
    if (profile != 1) {
        adts_set_err(err, err_len, "不支持的 ADTS profile=%u（仅 AAC-LC）",
                     (unsigned)profile);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* byte6 bits1..0：number_of_raw_data_blocks_in_frame，实际块数 = 字段值 + 1 */
    uint8_t num_blocks = (uint8_t)((h[6] & 0x03u) + 1u);
    if (num_blocks != 1) {
        /* 一个 ADTS 帧含多个 raw block 时 Helix 需要一次喂多块，本项目不支持 */
        adts_set_err(err, err_len, "ADTS 每帧 %u 个 raw block 不支持（仅 1）",
                     (unsigned)num_blocks);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* byte2 bits5..2：sampling_frequency_index（4bit），不是跨 byte2/3 的拼法 */
    uint32_t sr_idx = (uint32_t)((h[2] >> 2) & 0x0Fu);
    if (sr_idx >= 12) {
        adts_set_err(err, err_len, "ADTS 采样率索引 %u 越界", (unsigned)sr_idx);
        return ESP_ERR_INVALID_DATA;
    }
    uint32_t sr = adts_samp_rate_tab[sr_idx];
    if (sr != 32000 && sr != 44100 && sr != 48000) {
        adts_set_err(err, err_len, "AAC 采样率 %u 不支持（仅 32/44.1/48k）",
                     (unsigned)sr);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* byte2 bit0 + byte3 bits7..6：channel_configuration（共 3bit） */
    uint8_t ch = (uint8_t)(((h[2] & 0x01u) << 2) | ((h[3] >> 6) & 0x03u));
    if (ch != 1 && ch != 2) {
        adts_set_err(err, err_len, "ADTS 声道配置 %u 不支持（仅 1/2）",
                     (unsigned)ch);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t frame_len = (uint32_t)(((h[3] & 0x03u) << 11) |
                                    ((uint32_t)h[4] << 3) |
                                    ((h[5] >> 5) & 0x07u));
    uint8_t header_bytes = (h[1] & 0x01u) ? ADTS_HEADER_BYTES : 9u;
    if (frame_len <= header_bytes || frame_len > ADTS_FRAME_MAX) {
        adts_set_err(err, err_len, "ADTS 帧长 %u 非法", (unsigned)frame_len);
        return ESP_ERR_INVALID_SIZE;
    }
    out->sample_rate = sr;
    out->channels = ch;
    out->profile = profile;
    out->frame_length = frame_len;
    out->header_bytes = header_bytes;
    out->num_blocks = num_blocks;
    return ESP_OK;
}

static bool adts_header_same(const adts_header_t *a, const adts_header_t *b)
{
    return a->sample_rate == b->sample_rate && a->channels == b->channels &&
           a->profile == b->profile;
}

/* 尾部 ID3v1("TAG") / ID3v2("ID3") 标签识别。
   合法 ADTS 帧头固定以 FF Fx 开头，不可能与这两个 ASCII 前缀混淆。 */
static bool adts_is_trailing_tag(FILE *fp, uint64_t off, uint64_t file_size)
{
    uint8_t tag[3] = {0};
    if (off >= file_size || file_size - off < sizeof(tag)) {
        return false;
    }
    if (fseek(fp, (long)off, SEEK_SET) != 0 ||
        fread(tag, 1, sizeof(tag), fp) != sizeof(tag)) {
        return false;
    }
    return (memcmp(tag, "TAG", sizeof(tag)) == 0) ||
           (memcmp(tag, "ID3", sizeof(tag)) == 0);
}

esp_err_t adts_open(FILE *fp, adts_state_t *st, char *err, size_t err_len)
{
    if (fp == NULL || st == NULL) {
        adts_set_err(err, err_len, "ADTS 打开参数为空");
        return ESP_ERR_INVALID_ARG;
    }
    memset(st, 0, sizeof(*st));
    st->fp = fp;
    if (fseek(fp, 0, SEEK_END) != 0) {
        adts_set_err(err, err_len, "ADTS 读取文件长度失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    long sz = ftell(fp);
    if (sz < (long)(ADTS_HEADER_BYTES * 2)) {
        adts_set_err(err, err_len, "ADTS 文件过小");
        return ESP_ERR_INVALID_SIZE;
    }
    st->file_size = (uint64_t)sz;

    /* 一次读入前缀（PSRAM），找第一个能完整解析的 ADTS 头 */
    size_t scan_len = (st->file_size < ADTS_SYNC_SCAN_MAX) ?
                      (size_t)st->file_size : ADTS_SYNC_SCAN_MAX;
    uint8_t *prefix = heap_caps_malloc(scan_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (prefix == NULL) {
        adts_set_err(err, err_len, "ADTS 同步扫描缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(prefix, 1, scan_len, fp) != scan_len) {
        free(prefix);
        adts_set_err(err, err_len, "ADTS 读取文件头失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    adts_header_t first = {0};
    bool have_first = false;
    uint64_t first_off = 0;
    for (size_t i = 0; i + ADTS_HEADER_BYTES <= scan_len; i++) {
        if (prefix[i] != 0xFF || (prefix[i + 1] & 0xF0u) != 0xF0u) {
            continue;
        }
        uint8_t h[ADTS_HEADER_BYTES];
        memcpy(h, prefix + i, sizeof(h));
        if (adts_parse_header(h, &first, err, err_len) == ESP_OK) {
            first_off = (uint64_t)i;
            have_first = true;
            break;
        }
    }
    free(prefix);
    if (!have_first) {
        adts_set_err(err, err_len, "未找到有效 ADTS 帧头（伪装扩展名或格式不支持）");
        return ESP_ERR_NOT_FOUND;
    }
    if (first_off + first.frame_length > st->file_size) {
        adts_set_err(err, err_len, "首个 ADTS 帧超出文件长度");
        return ESP_ERR_INVALID_SIZE;
    }

    /* 全文件轻量帧计数：每帧只读 7 字节头，fseek 跳过帧体 */
    uint64_t off = first_off;
    uint32_t frames = 0;
    uint8_t h[ADTS_HEADER_BYTES];
    bool tail_tag = false;
    while (off + ADTS_HEADER_BYTES <= st->file_size) {
        /* 尾部 ID3v1/v2 标签不参与帧计数，直接停在这一帧之前 */
        if (adts_is_trailing_tag(fp, off, st->file_size)) {
            tail_tag = true;
            break;
        }
        if (fseek(fp, (long)off, SEEK_SET) != 0 ||
            fread(h, 1, sizeof(h), fp) != sizeof(h)) {
            adts_set_err(err, err_len, "ADTS 帧计数读取失败");
            return ESP_ERR_INVALID_RESPONSE;
        }
        adts_header_t hdr = {0};
        char dummy_err[96] = "";   /* 计数循环里不覆盖首个错误原因 */
        esp_err_t err2 = adts_parse_header(h, &hdr, dummy_err, sizeof(dummy_err));
        if (err2 != ESP_OK) {
            adts_set_err(err, err_len, "ADTS 第 %u 帧头损坏（偏移 %llu）",
                         (unsigned)frames, (unsigned long long)off);
            return err2;
        }
        if (!adts_header_same(&first, &hdr)) {
            adts_set_err(err, err_len, "ADTS 第 %u 帧参数与首帧不一致",
                         (unsigned)frames);
            return ESP_ERR_INVALID_DATA;
        }
        if (off + hdr.frame_length > st->file_size) {
            adts_set_err(err, err_len, "ADTS 第 %u 帧被截断", (unsigned)frames);
            return ESP_ERR_INVALID_SIZE;
        }
        off += hdr.frame_length;
        frames++;
        if (frames >= ADTS_MAX_FRAMES) {
            adts_set_err(err, err_len, "ADTS 帧数超过上限");
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (off > st->file_size) {
        adts_set_err(err, err_len, "ADTS 帧偏移越界");
        return ESP_ERR_INVALID_SIZE;
    }
    if (off < st->file_size) {
        if (tail_tag) {
            /* 已知标签：只提示一次，不参与帧计数，也不当作坏曲 */
            ESP_LOGW(TAG, "ADTS 尾部有 %llu 字节标签（TAG/ID3），已忽略",
                     (unsigned long long)(st->file_size - off));
        } else if (st->file_size - off >= ADTS_HEADER_BYTES) {
            /* 循环退出说明剩余数据不是合法帧头，且不是已知标签：按坏曲拒绝 */
            adts_set_err(err, err_len, "ADTS 尾部存在无法解析的数据");
            return ESP_ERR_INVALID_DATA;
        } else {
            /* 不足一个头部的填充/对齐残差，直接忽略 */
            ESP_LOGW(TAG, "ADTS 尾部有 %llu 字节非音频数据，已忽略",
                     (unsigned long long)(st->file_size - off));
        }
    }

    st->first_frame_offset = first_off;
    st->cur_offset = first_off;
    st->frames_total = frames;
    st->cur_frame = 0;
    st->sample_rate = first.sample_rate;
    st->channels = first.channels;
    st->profile = first.profile;
    st->aot = (uint8_t)(first.profile + 1u);   /* profile 1 → AOT 2 (LC) */
    return ESP_OK;
}

esp_err_t adts_read_frame(adts_state_t *st, uint8_t *buf, size_t cap, size_t *len)
{
    if (st == NULL || buf == NULL || len == NULL || st->fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *len = 0;
    if (st->cur_frame >= st->frames_total ||
        st->cur_offset + ADTS_HEADER_BYTES > st->file_size) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t h[ADTS_HEADER_BYTES];
    if (fseek(st->fp, (long)st->cur_offset, SEEK_SET) != 0 ||
        fread(h, 1, sizeof(h), st->fp) != sizeof(h)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    adts_header_t hdr = {0};
    char dummy_err[96] = "";
    if (adts_parse_header(h, &hdr, dummy_err, sizeof(dummy_err)) != ESP_OK) {
        return ESP_ERR_INVALID_DATA;   /* 损坏帧：不返回任何数据 */
    }
    if (hdr.sample_rate != st->sample_rate || hdr.channels != st->channels ||
        hdr.profile != st->profile) {
        return ESP_ERR_INVALID_DATA;
    }
    if (hdr.frame_length > cap || st->cur_offset + hdr.frame_length > st->file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 从帧头开始读整帧，Helix 自己解析 ADTS 头 */
    if (fseek(st->fp, (long)st->cur_offset, SEEK_SET) != 0 ||
        fread(buf, 1, hdr.frame_length, st->fp) != hdr.frame_length) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    st->cur_offset += hdr.frame_length;
    st->cur_frame++;
    *len = hdr.frame_length;
    return ESP_OK;
}

esp_err_t adts_seek_seconds(adts_state_t *st, uint32_t seconds)
{
    if (st == NULL || st->fp == NULL || st->frames_total == 0 || st->sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 目标帧号 = 秒数 × 采样率 / 每帧样本数 */
    uint64_t target_frame = (uint64_t)seconds * st->sample_rate / ADTS_SAMPLES_PER_FRAME;
    if (target_frame >= st->frames_total) {
        target_frame = st->frames_total - 1;
    }
    uint64_t audio_bytes = st->file_size - st->first_frame_offset;
    uint64_t probe = st->first_frame_offset +
                     audio_bytes * target_frame / st->frames_total;
    if (probe + ADTS_HEADER_BYTES > st->file_size) {
        probe = st->file_size - ADTS_HEADER_BYTES;
    }

    /* 从落点向后扫描字节流重同步：最多 ADTS_RESYNC_MAX_FRAMES 次尝试、
       ADTS_RESYNC_SCAN_BYTES 字节，避免损坏文件里无限找帧 */
    if (fseek(st->fp, (long)probe, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t h[ADTS_HEADER_BYTES];
    uint32_t attempts = 0;
    uint64_t scanned = 0;
    while (scanned + ADTS_HEADER_BYTES <= ADTS_RESYNC_SCAN_BYTES &&
           attempts < ADTS_RESYNC_MAX_FRAMES) {
        int c = fgetc(st->fp);
        if (c == EOF) {
            break;
        }
        scanned++;
        if (c != 0xFF) {
            continue;
        }
        h[0] = (uint8_t)c;
        if (fread(h + 1, 1, ADTS_HEADER_BYTES - 1, st->fp) != ADTS_HEADER_BYTES - 1) {
            break;
        }
        scanned += ADTS_HEADER_BYTES - 1;
        attempts++;
        adts_header_t hdr = {0};
        char dummy_err[96] = "";
        if (adts_parse_header(h, &hdr, dummy_err, sizeof(dummy_err)) == ESP_OK &&
            hdr.sample_rate == st->sample_rate && hdr.channels == st->channels &&
            hdr.profile == st->profile) {
            uint64_t found = probe + scanned - ADTS_HEADER_BYTES;
            if (found + hdr.frame_length <= st->file_size) {
                st->cur_offset = found;
                st->cur_frame = target_frame;   /* 近似值，仅用于状态一致性 */
                return ESP_OK;
            }
        }
    }
    /* 重同步失败：不改变游标，由上层继续播放当前位置 */
    return ESP_ERR_NOT_FOUND;
}

uint64_t adts_total_frames(const adts_state_t *st)
{
    return st ? st->frames_total : 0;
}

void adts_close(adts_state_t *st)
{
    if (st == NULL) {
        return;
    }
    st->fp = NULL;
    st->file_size = 0;
    st->first_frame_offset = 0;
    st->cur_offset = 0;
    st->frames_total = 0;
    st->cur_frame = 0;
    st->sample_rate = 0;
    st->channels = 0;
    st->profile = 0;
    st->aot = 0;
}
