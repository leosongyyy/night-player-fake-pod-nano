/**
 * MP4/M4A 轻量解封装 —— 只解析 AAC-LC 播放必需的 box
 *
 * 设计约束：不引 mp4ff/ffmpeg/gpac；所有表放 PSRAM；逐 sample 读 mdat，
 * 不整文件缓存；任何长度异常都在打开阶段拒绝，避免坏曲播成满幅噪声。
 * box 遍历借鉴 wav_scan_chunks 的 next<=pos 防御，绝不死循环。
 */

#include "core/audio_decoders/mp4_demux.h"

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

static const char *TAG = "mp4_demux";

/* 单曲 sample 数上限：5 分钟 44.1k AAC 约 1.3 万，100 万足够且能挡住伪造长度 */
#define MP4_MAX_SAMPLES       (1000000u)
/* stsz/stco 单项 4 字节，单表最多 4MB，避免损坏文件诱导超大 PSRAM 分配 */
#define MP4_MAX_TABLE_BYTES   (4u * 1024u * 1024u)
/* covr 封面上限，与 cover_art.c 的 CA_MAX_JPEG 一致 */
#define MP4_MAX_COVER_BYTES   (2u * 1024u * 1024u)
/* esds 描述符最多解析 64 层/64 个，防嵌套炸弹 */
#define MP4_DESC_MAX_DEPTH    64
/* AudioSpecificConfig 采样率表（与 ISO 14496-3 一致，索引 0~11） */
static const uint32_t mp4_samp_rate_tab[12] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000,
};

typedef struct {
    uint64_t offset;   /* box 数据区（body）起点 */
    uint64_t size;     /* box 数据区长度 */
} mp4_box_t;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int bit_pos;
} mp4_bits_t;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void mp4_set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void *mp4_psram_calloc(size_t n, size_t sz)
{
    if (n != 0 && sz > SIZE_MAX / n) {
        return NULL;
    }
    return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static uint32_t mp4_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t mp4_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t mp4_box_type(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* 读取 pos 处 box 的 header；end 为父 box 数据区结束位置。
   box->offset 指向 payload 起点，box->size 表示 payload 长度（不含 header）。 */
static esp_err_t mp4_box_header(FILE *fp, uint64_t pos, uint64_t end,
                                uint32_t *type, mp4_box_t *box)
{
    if (pos > end || end - pos < 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t hdr[8];
    if (fseek(fp, (long)pos, SEEK_SET) != 0 ||
        fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint64_t size = mp4_be32(hdr);
    *type = mp4_be32(hdr + 4);
    uint64_t header_size = 8;
    if (size == 1) {
        uint8_t ext[8];
        if (end - pos < 16 ||
            fread(ext, 1, sizeof(ext), fp) != sizeof(ext)) {
            return ESP_ERR_INVALID_SIZE;
        }
        size = mp4_be64(ext);
        header_size = 16;
    } else if (size == 0) {
        size = end - pos;   /* 延伸到父 box 末尾 */
    }
    /* size 是含 header 的总长度；这里按 payload 长度做边界检查。 */
    if (size < header_size || size > end - pos) {
        return ESP_ERR_INVALID_SIZE;
    }
    box->offset = pos + header_size;
    box->size = size - header_size;
    return ESP_OK;
}

/* 从 *cursor 开始找 type；同时把 *cursor 推进到下一个兄弟 box */
static esp_err_t mp4_box_next(FILE *fp, uint64_t *cursor, uint64_t end,
                              uint32_t type, mp4_box_t *found)
{
    while (*cursor + 8 <= end) {
        uint32_t got_type = 0;
        mp4_box_t box = {0};
        uint64_t pos = *cursor;
        esp_err_t err = mp4_box_header(fp, pos, end, &got_type, &box);
        if (err != ESP_OK) {
            return err;
        }
        uint64_t next = box.offset + box.size;
        if (next <= pos) {
            return ESP_ERR_INVALID_SIZE;   /* 防死循环 */
        }
        *cursor = next;
        if (got_type == type) {
            *found = box;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t mp4_box_find(FILE *fp, uint64_t start, uint64_t end,
                              uint32_t type, mp4_box_t *found)
{
    uint64_t cursor = start;
    return mp4_box_next(fp, &cursor, end, type, found);
}

static esp_err_t mp4_read_u32_array(FILE *fp, uint64_t offset, uint32_t count,
                                    uint32_t **out)
{
    if (count == 0 || count > MP4_MAX_SAMPLES || (uint64_t)count * 4 > MP4_MAX_TABLE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 先按字节读入再逐项 mp4_be32：直接 fread 到 uint32 数组会依赖主机字节序 */
    uint8_t *raw = mp4_psram_calloc(count, 4);
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0 ||
        fread(raw, 4, count, fp) != count) {
        free(raw);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t *arr = mp4_psram_calloc(count, sizeof(uint32_t));
    if (arr == NULL) {
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        arr[i] = mp4_be32(raw + (size_t)i * 4);
    }
    free(raw);
    *out = arr;
    return ESP_OK;
}

static esp_err_t mp4_read_co64(FILE *fp, uint64_t offset, uint32_t count,
                               uint64_t **out)
{
    if (count == 0 || count > MP4_MAX_SAMPLES || (uint64_t)count * 8 > MP4_MAX_TABLE_BYTES * 2u) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raw = mp4_psram_calloc(count, 8);
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0 ||
        fread(raw, 8, count, fp) != count) {
        free(raw);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint64_t *arr = mp4_psram_calloc(count, sizeof(uint64_t));
    if (arr == NULL) {
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        arr[i] = mp4_be64(raw + (size_t)i * 8);
    }
    free(raw);
    *out = arr;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* esds → AudioSpecificConfig                                          */
/* ------------------------------------------------------------------ */

/* ISO BMFF descriptor：1 字节 tag + 1~4 字节变长长度（高位置 1 表示继续） */
static bool mp4_desc_next(const uint8_t **cursor, const uint8_t *end,
                          uint8_t *tag, const uint8_t **payload, uint32_t *len)
{
    const uint8_t *p = *cursor;
    if (p == NULL || end == NULL || p >= end) {
        return false;
    }
    *tag = *p++;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        if (p >= end) {
            return false;
        }
        uint8_t b = *p++;
        value = (value << 7) | (b & 0x7Fu);
        if ((b & 0x80u) == 0) {
            break;
        }
        if (i == 3) {
            return false;   /* 超过 4 字节长度字段，视为损坏 */
        }
    }
    if (value > (uint32_t)(end - p)) {
        return false;
    }
    *payload = p;
    *len = value;
    *cursor = p + value;
    return true;
}

static bool mp4_desc_find(const uint8_t *start, const uint8_t *end, uint8_t want,
                          const uint8_t **payload, uint32_t *len)
{
    const uint8_t *cursor = start;
    for (int i = 0; i < MP4_DESC_MAX_DEPTH; i++) {
        uint8_t tag = 0;
        const uint8_t *p = NULL;
        uint32_t n = 0;
        if (!mp4_desc_next(&cursor, end, &tag, &p, &n)) {
            return false;
        }
        if (tag == want) {
            *payload = p;
            *len = n;
            return true;
        }
    }
    return false;
}

static uint32_t mp4_asc_bits(mp4_bits_t *br, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (br->p >= br->end) {
            return 0;   /* 长度已在调用处校验，这里是兜底 */
        }
        v = (v << 1) | ((*br->p >> (7 - br->bit_pos)) & 1u);
        if (++br->bit_pos == 8) {
            br->bit_pos = 0;
            br->p++;
        }
    }
    return v;
}

/* 解析 ASC，只放行 AOT=2（AAC-LC）+ 32/44.1/48k + 单/双声道 */
static esp_err_t mp4_parse_asc(const uint8_t *asc, size_t asc_len,
                               uint32_t *sample_rate, uint8_t *channels,
                               uint8_t *aot, char *err, size_t err_len)
{
    if (asc == NULL || asc_len < 2) {
        mp4_set_err(err, err_len, "AudioSpecificConfig 过短");
        return ESP_ERR_INVALID_SIZE;
    }
    mp4_bits_t br = {asc, asc + asc_len, 0};
    uint32_t object_type = mp4_asc_bits(&br, 5);
    uint32_t sr_idx = mp4_asc_bits(&br, 4);
    uint32_t channel_cfg = mp4_asc_bits(&br, 4);
    uint32_t sr = 0;

    if (object_type != 2) {
        mp4_set_err(err, err_len, "不支持的 AAC 配置 AOT=%u（仅 AAC-LC=2；SBR/PS 拒绝）",
                    (unsigned)object_type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sr_idx == 0x0F) {
        sr = mp4_asc_bits(&br, 24);
    } else if (sr_idx < 12) {
        sr = mp4_samp_rate_tab[sr_idx];
    }
    if (sr != 32000 && sr != 44100 && sr != 48000) {
        mp4_set_err(err, err_len, "AAC 采样率 %u 不支持（仅 32/44.1/48k）",
                    (unsigned)sr);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (channel_cfg != 1 && channel_cfg != 2) {
        mp4_set_err(err, err_len, "AAC 声道配置 %u 不支持（仅 1/2）",
                    (unsigned)channel_cfg);
        return ESP_ERR_NOT_SUPPORTED;
    }
    *sample_rate = sr;
    *channels = (uint8_t)channel_cfg;
    *aot = 2;
    return ESP_OK;
}

static esp_err_t mp4_parse_esds(const uint8_t *data, size_t len,
                                uint8_t **asc_out, size_t *asc_len_out,
                                char *err, size_t err_len)
{
    if (data == NULL || len < 4) {
        mp4_set_err(err, err_len, "esds 块过短");
        return ESP_ERR_INVALID_SIZE;
    }
    /* 前 4 字节是 FullBox(version+flags) */
    const uint8_t *end = data + len;
    const uint8_t *es = NULL;
    uint32_t es_len = 0;
    if (!mp4_desc_find(data + 4, end, 0x03, &es, &es_len) || es == NULL) {
        mp4_set_err(err, err_len, "esds 缺少 ES_Descriptor");
        return ESP_ERR_INVALID_DATA;
    }
    const uint8_t *es_end = es + es_len;
    /* ES_Descriptor: ES_ID(2) + flags(1)，其后才是子描述符 */
    const uint8_t *dcd = NULL;
    uint32_t dcd_len = 0;
    if (es_len < 3 ||
        !mp4_desc_find(es + 3, es_end, 0x04, &dcd, &dcd_len) || dcd == NULL) {
        mp4_set_err(err, err_len, "esds 缺少 DecoderConfigDescriptor");
        return ESP_ERR_INVALID_DATA;
    }
    if (dcd_len < 13) {
        mp4_set_err(err, err_len, "DecoderConfigDescriptor 过短");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t object_type_indication = dcd[0];
    /* 0x40=MPEG-4 AAC，0x66=MPEG-2 AAC LC；0x67 是 MPEG-2 SSR，拒绝 */
    if (object_type_indication != 0x40 && object_type_indication != 0x66) {
        mp4_set_err(err, err_len, "音轨编码 0x%02X 不是 AAC-LC",
                    (unsigned)object_type_indication);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t *dcd_end = dcd + dcd_len;
    const uint8_t *dsi = NULL;
    uint32_t dsi_len = 0;
    /* DecoderConfigDescriptor 固定 13 字节之后是 DecSpecificInfo(0x05) 等 */
    if (!mp4_desc_find(dcd + 13, dcd_end, 0x05, &dsi, &dsi_len) ||
        dsi == NULL || dsi_len == 0) {
        mp4_set_err(err, err_len, "esds 缺少 DecSpecificInfo");
        return ESP_ERR_INVALID_DATA;
    }
    uint8_t *copy = mp4_psram_calloc(dsi_len, 1);
    if (copy == NULL) {
        mp4_set_err(err, err_len, "AudioSpecificConfig 分配失败");
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, dsi, dsi_len);
    *asc_out = copy;
    *asc_len_out = dsi_len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* sample 表读取与定位                                                 */
/* ------------------------------------------------------------------ */

static uint64_t mp4_chunk_offset(const mp4_track_t *t, uint32_t chunk)
{
    if (chunk >= t->chunk_count) {
        return 0;
    }
    return t->use_co64 ? t->co64[chunk] : (uint64_t)t->stco[chunk];
}

static uint32_t mp4_chunk_samples(const mp4_track_t *t, uint32_t chunk)
{
    uint32_t samples = 1;   /* stsc 缺失时按每 chunk 一个 sample 兜底 */
    for (uint32_t i = 0; i < t->stsc_count; i++) {
        if (t->stsc[i].first_chunk <= chunk + 1) {
            samples = t->stsc[i].samples_per_chunk;
        } else {
            break;
        }
    }
    return samples;
}

static uint32_t mp4_sample_size(const mp4_track_t *t, uint32_t idx)
{
    if (idx >= t->sample_count) {
        return 0;
    }
    return t->fixed_sample_size ? t->fixed_sample_size : t->stsz[idx];
}

static void mp4_cursor_reset(mp4_track_t *t)
{
    t->cur_chunk = 0;
    t->cur_chunk_start_sample = 0;
    t->cur_chunk_samples = mp4_chunk_samples(t, 0);
    t->cur_chunk_offset = mp4_chunk_offset(t, 0);
}

/* 把游标移动到包含 sample_idx 的 chunk；seek 时会重算 chunk 内偏移 */
static esp_err_t mp4_cursor_seek(mp4_track_t *t, uint32_t sample_idx)
{
    mp4_cursor_reset(t);
    if (t->chunk_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    while (sample_idx >= t->cur_chunk_start_sample + t->cur_chunk_samples) {
        t->cur_chunk_start_sample += t->cur_chunk_samples;
        t->cur_chunk++;
        if (t->cur_chunk >= t->chunk_count) {
            return ESP_ERR_NOT_FOUND;
        }
        t->cur_chunk_samples = mp4_chunk_samples(t, t->cur_chunk);
    }
    uint64_t off = mp4_chunk_offset(t, t->cur_chunk);
    for (uint32_t i = t->cur_chunk_start_sample; i < sample_idx; i++) {
        off += mp4_sample_size(t, i);
    }
    t->cur_chunk_offset = off;
    return ESP_OK;
}

static esp_err_t mp4_locate_sample(mp4_track_t *t, uint32_t idx,
                                   uint64_t *offset, uint32_t *size)
{
    if (idx >= t->sample_count) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 顺序读时 idx 单调增；seek 后游标可能被重置，这里做兼容判断 */
    if (idx < t->cur_chunk_start_sample) {
        mp4_cursor_reset(t);
    }
    while (idx >= t->cur_chunk_start_sample + t->cur_chunk_samples) {
        t->cur_chunk_start_sample += t->cur_chunk_samples;
        t->cur_chunk++;
        if (t->cur_chunk >= t->chunk_count) {
            return ESP_ERR_INVALID_SIZE;
        }
        t->cur_chunk_samples = mp4_chunk_samples(t, t->cur_chunk);
        t->cur_chunk_offset = mp4_chunk_offset(t, t->cur_chunk);
    }
    uint64_t off = mp4_chunk_offset(t, t->cur_chunk);
    for (uint32_t i = t->cur_chunk_start_sample; i < idx; i++) {
        off += mp4_sample_size(t, i);
    }
    *offset = off;
    *size = mp4_sample_size(t, idx);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* stbl / stsd 解析                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t mp4_read_stbl(FILE *fp, mp4_track_t *t, mp4_box_t stbl,
                               uint32_t *sample_rate, uint8_t *channels,
                               uint8_t *aot, char *err, size_t err_len)
{
    const uint64_t end = stbl.offset + stbl.size;

    /* ---- stsd → mp4a → esds ---- */
    mp4_box_t stsd = {0};
    esp_err_t err2 = mp4_box_find(fp, stbl.offset, end,
                                  mp4_box_type('s', 't', 's', 'd'), &stsd);
    if (err2 != ESP_OK) {
        mp4_set_err(err, err_len, "stbl 缺少 stsd");
        return ESP_ERR_NOT_FOUND;
    }
    if (stsd.size < 8) {
        mp4_set_err(err, err_len, "stsd 块过短");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t hdr[8];
    if (fseek(fp, (long)stsd.offset, SEEK_SET) != 0 ||
        fread(hdr, 1, 8, fp) != 8) {
        mp4_set_err(err, err_len, "读取 stsd 失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t entries = mp4_be32(hdr + 4);
    if (entries == 0 || entries > 64) {
        mp4_set_err(err, err_len, "stsd 条目数异常：%u", (unsigned)entries);
        return ESP_ERR_INVALID_SIZE;
    }

    /* stsd 自身有明确边界，条目不能超过 stsd，否则会误读后面的 stts/stsz */
    uint64_t stsd_end = stsd.offset + stsd.size;
    uint64_t pos = stsd.offset + 8;
    mp4_box_t esds = {0};
    bool have_esds = false;
    for (uint32_t i = 0; i < entries && pos + 8 <= stsd_end; i++) {
        uint32_t type = 0;
        mp4_box_t entry = {0};
        err2 = mp4_box_header(fp, pos, stsd_end, &type, &entry);
        if (err2 != ESP_OK) {
            mp4_set_err(err, err_len, "stsd 条目头损坏");
            return err2;
        }
        if (type == mp4_box_type('m', 'p', '4', 'a')) {
            /* AudioSampleEntry 固定头 28 字节：8(size+type)+6 reserved+
               2 dataRefIndex+20 音频字段；子 box 从 +28 开始 */
            uint64_t child = entry.offset + 28;
            uint64_t child_end = entry.offset + entry.size;
            if (child > child_end) {
                mp4_set_err(err, err_len, "mp4a 条目过短");
                return ESP_ERR_INVALID_SIZE;
            }
            err2 = mp4_box_find(fp, child, child_end,
                                mp4_box_type('e', 's', 'd', 's'), &esds);
            if (err2 == ESP_OK) {
                have_esds = true;
                break;
            }
        }
        uint64_t next = entry.offset + entry.size;
        if (next <= pos) {
            break;
        }
        pos = next;
    }
    if (!have_esds) {
        mp4_set_err(err, err_len, "音轨缺少 mp4a/esds（不是 AAC 音轨）");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (esds.size > 4096) {
        mp4_set_err(err, err_len, "esds 块过大：%llu", (unsigned long long)esds.size);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *esds_buf = mp4_psram_calloc(1, (size_t)esds.size);
    if (esds_buf == NULL) {
        mp4_set_err(err, err_len, "esds 缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, (long)esds.offset, SEEK_SET) != 0 ||
        fread(esds_buf, 1, (size_t)esds.size, fp) != esds.size) {
        free(esds_buf);
        mp4_set_err(err, err_len, "读取 esds 失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t *asc = NULL;
    size_t asc_len = 0;
    err2 = mp4_parse_esds(esds_buf, (size_t)esds.size, &asc, &asc_len,
                          err, err_len);
    free(esds_buf);
    if (err2 != ESP_OK) {
        return err2;
    }
    uint32_t sr = 0;
    uint8_t ch = 0;
    uint8_t aot_value = 0;
    err2 = mp4_parse_asc(asc, asc_len, &sr, &ch, &aot_value, err, err_len);
    if (err2 != ESP_OK) {
        free(asc);
        return err2;
    }
    t->asc = asc;
    t->asc_len = asc_len;
    *sample_rate = sr;
    *channels = ch;
    *aot = aot_value;

    /* ---- stts ---- */
    mp4_box_t stts = {0};
    err2 = mp4_box_find(fp, stbl.offset, end, mp4_box_type('s', 't', 't', 's'), &stts);
    if (err2 != ESP_OK || stts.size < 8) {
        mp4_set_err(err, err_len, "stts 缺失或过短");
        return ESP_ERR_INVALID_SIZE;
    }
    /* sh 需容纳 stsz 的 12 字节头（4B version/flags + 4B sample_size + 4B sample_count），
     * 同时兼容 stts 的 8 字节头；原 8 字节会在 stsz 分支被 mp4_be32(sh+8) 越界读 4 字节 */
    uint8_t sh[12];
    if (fseek(fp, (long)stts.offset, SEEK_SET) != 0 ||
        fread(sh, 1, 8, fp) != 8) {
        mp4_set_err(err, err_len, "读取 stts 失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->stts_count = mp4_be32(sh + 4);
    if (t->stts_count == 0 || t->stts_count > MP4_MAX_SAMPLES ||
        (uint64_t)t->stts_count * 8 > stts.size - 8) {
        mp4_set_err(err, err_len, "stts 条目数异常：%u", (unsigned)t->stts_count);
        return ESP_ERR_INVALID_SIZE;
    }
    t->stts = mp4_psram_calloc(t->stts_count, sizeof(mp4_stts_entry_t));
    if (t->stts == NULL) {
        mp4_set_err(err, err_len, "stts 表分配失败");
        return ESP_ERR_NO_MEM;
    }
    uint8_t *raww = mp4_psram_calloc(t->stts_count, 8);
    if (raww == NULL) {
        mp4_set_err(err, err_len, "stts 临时缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    if (fread(raww, 8, t->stts_count, fp) != t->stts_count) {
        free(raww);
        mp4_set_err(err, err_len, "读取 stts 表失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (uint32_t i = 0; i < t->stts_count; i++) {
        t->stts[i].sample_count = mp4_be32(raww + (size_t)i * 8);
        t->stts[i].sample_delta = mp4_be32(raww + (size_t)i * 8 + 4);
    }
    free(raww);

    /* ---- stsz ---- */
    mp4_box_t stsz = {0};
    err2 = mp4_box_find(fp, stbl.offset, end, mp4_box_type('s', 't', 's', 'z'), &stsz);
    if (err2 != ESP_OK || stsz.size < 12) {
        mp4_set_err(err, err_len, "stsz 缺失或过短");
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, (long)stsz.offset, SEEK_SET) != 0 ||
        fread(sh, 1, 12, fp) != 12) {
        mp4_set_err(err, err_len, "读取 stsz 失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->fixed_sample_size = mp4_be32(sh + 4);
    t->sample_count = mp4_be32(sh + 8);
    if (t->sample_count == 0 || t->sample_count > MP4_MAX_SAMPLES) {
        mp4_set_err(err, err_len, "sample 数异常：%u", (unsigned)t->sample_count);
        return ESP_ERR_INVALID_SIZE;
    }
    if (t->fixed_sample_size == 0) {
        if ((uint64_t)t->sample_count * 4 > stsz.size - 12) {
            mp4_set_err(err, err_len, "stsz 表长度不足");
            return ESP_ERR_INVALID_SIZE;
        }
        err2 = mp4_read_u32_array(fp, stsz.offset + 12, t->sample_count, &t->stsz);
        if (err2 != ESP_OK) {
            mp4_set_err(err, err_len, "stsz 表读取失败");
            return err2;
        }
    }

    /* ---- stco / co64 ---- */
    mp4_box_t stco = {0};
    mp4_box_t co64 = {0};
    err2 = mp4_box_find(fp, stbl.offset, end, mp4_box_type('s', 't', 'c', 'o'), &stco);
    if (err2 == ESP_OK) {
        t->use_co64 = false;
    } else {
        err2 = mp4_box_find(fp, stbl.offset, end, mp4_box_type('c', 'o', '6', '4'), &co64);
        if (err2 != ESP_OK) {
            mp4_set_err(err, err_len, "stco/co64 均缺失");
            return ESP_ERR_NOT_FOUND;
        }
        t->use_co64 = true;
    }
    mp4_box_t chunks = t->use_co64 ? co64 : stco;
    if (chunks.size < 8) {
        mp4_set_err(err, err_len, "chunk 偏移表过短");
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, (long)chunks.offset, SEEK_SET) != 0 ||
        fread(sh, 1, 8, fp) != 8) {
        mp4_set_err(err, err_len, "读取 chunk 偏移表失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->chunk_count = mp4_be32(sh + 4);
    if (t->chunk_count == 0 || t->chunk_count > MP4_MAX_SAMPLES) {
        mp4_set_err(err, err_len, "chunk 数异常：%u", (unsigned)t->chunk_count);
        return ESP_ERR_INVALID_SIZE;
    }
    if (t->use_co64) {
        if ((uint64_t)t->chunk_count * 8 > chunks.size - 8) {
            mp4_set_err(err, err_len, "co64 表长度不足");
            return ESP_ERR_INVALID_SIZE;
        }
        err2 = mp4_read_co64(fp, chunks.offset + 8, t->chunk_count, &t->co64);
    } else {
        if ((uint64_t)t->chunk_count * 4 > chunks.size - 8) {
            mp4_set_err(err, err_len, "stco 表长度不足");
            return ESP_ERR_INVALID_SIZE;
        }
        err2 = mp4_read_u32_array(fp, chunks.offset + 8, t->chunk_count, &t->stco);
    }
    if (err2 != ESP_OK) {
        mp4_set_err(err, err_len, "chunk 偏移表读取失败");
        return err2;
    }

    /* ---- stsc ---- */
    mp4_box_t stsc = {0};
    err2 = mp4_box_find(fp, stbl.offset, end, mp4_box_type('s', 't', 's', 'c'), &stsc);
    if (err2 != ESP_OK || stsc.size < 8) {
        mp4_set_err(err, err_len, "stsc 缺失或过短");
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, (long)stsc.offset, SEEK_SET) != 0 ||
        fread(sh, 1, 8, fp) != 8) {
        mp4_set_err(err, err_len, "读取 stsc 失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->stsc_count = mp4_be32(sh + 4);
    if (t->stsc_count == 0 || t->stsc_count > MP4_MAX_SAMPLES ||
        (uint64_t)t->stsc_count * 12 > stsc.size - 8) {
        mp4_set_err(err, err_len, "stsc 条目数异常：%u", (unsigned)t->stsc_count);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raww2 = mp4_psram_calloc(t->stsc_count, 12);
    if (raww2 == NULL) {
        mp4_set_err(err, err_len, "stsc 临时缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    if (fread(raww2, 12, t->stsc_count, fp) != t->stsc_count) {
        free(raww2);
        mp4_set_err(err, err_len, "读取 stsc 表失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->stsc = mp4_psram_calloc(t->stsc_count, sizeof(mp4_stsc_entry_t));
    if (t->stsc == NULL) {
        free(raww2);
        mp4_set_err(err, err_len, "stsc 表分配失败");
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < t->stsc_count; i++) {
        t->stsc[i].first_chunk = mp4_be32(raww2 + (size_t)i * 12);
        t->stsc[i].samples_per_chunk = mp4_be32(raww2 + (size_t)i * 12 + 4);
        if (t->stsc[i].samples_per_chunk == 0) {
            free(raww2);
            mp4_set_err(err, err_len, "stsc 每 chunk sample 数为 0");
            return ESP_ERR_INVALID_DATA;
        }
    }
    free(raww2);

    /* 校验 stts 覆盖的 sample 数与 stsz 一致，避免时长/进度错位 */
    uint64_t stts_samples = 0;
    for (uint32_t i = 0; i < t->stts_count; i++) {
        stts_samples += t->stts[i].sample_count;
    }
    if (stts_samples < t->sample_count) {
        mp4_set_err(err, err_len, "stts 未覆盖全部 sample");
        return ESP_ERR_INVALID_DATA;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t mp4_demux_open(FILE *fp, mp4_track_t **out, char *err, size_t err_len)
{
    if (fp == NULL || out == NULL) {
        mp4_set_err(err, err_len, "MP4 打开参数为空");
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        mp4_set_err(err, err_len, "MP4 读取文件长度失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    long fsize = ftell(fp);
    if (fsize < 16) {
        mp4_set_err(err, err_len, "MP4 文件过小");
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t file_size = (uint64_t)fsize;

    /* 顶层：找 moov 与 mdat（两者顺序任意） */
    uint64_t cursor = 0;
    mp4_box_t moov = {0};
    mp4_box_t mdat = {0};
    bool have_moov = false;
    bool have_mdat = false;
    while (cursor + 8 <= file_size) {
        uint32_t type = 0;
        mp4_box_t box = {0};
        esp_err_t err2 = mp4_box_header(fp, cursor, file_size, &type, &box);
        if (err2 != ESP_OK) {
            mp4_set_err(err, err_len, "MP4 box 头损坏");
            return err2;
        }
        uint64_t next = box.offset + box.size;
        if (next <= cursor) {
            mp4_set_err(err, err_len, "MP4 box 长度非法（防死循环）");
            return ESP_ERR_INVALID_SIZE;
        }
        cursor = next;
        if (type == mp4_box_type('m', 'o', 'o', 'v')) {
            moov = box;
            have_moov = true;
        } else if (type == mp4_box_type('m', 'd', 'a', 't')) {
            mdat = box;
            have_mdat = true;
        }
    }
    if (!have_moov || !have_mdat) {
        mp4_set_err(err, err_len, "不是有效的 MP4/M4A（缺 moov 或 mdat）");
        return ESP_ERR_NOT_FOUND;
    }

    /* moov 里逐 trak 找 soun 音轨 */
    mp4_track_t *t = mp4_psram_calloc(1, sizeof(*t));
    if (t == NULL) {
        mp4_set_err(err, err_len, "MP4 轨道句柄分配失败");
        return ESP_ERR_NO_MEM;
    }
    t->fp = fp;
    t->mdat_offset = mdat.offset;
    t->mdat_size = mdat.size;

    uint64_t trak_cursor = moov.offset;
    uint64_t moov_end = moov.offset + moov.size;
    bool found = false;
    uint32_t sr = 0;
    uint8_t ch = 0;
    uint8_t aot = 0;
    while (trak_cursor + 8 <= moov_end) {
        uint32_t type = 0;
        mp4_box_t trak = {0};
        esp_err_t err2 = mp4_box_header(fp, trak_cursor, moov_end, &type, &trak);
        if (err2 != ESP_OK) {
            break;
        }
        uint64_t next = trak.offset + trak.size;
        if (next <= trak_cursor) {
            break;
        }
        trak_cursor = next;
        if (type != mp4_box_type('t', 'r', 'a', 'k')) {
            continue;
        }

        mp4_box_t mdia = {0};
        if (mp4_box_find(fp, trak.offset, trak.offset + trak.size,
                         mp4_box_type('m', 'd', 'i', 'a'), &mdia) != ESP_OK) {
            continue;
        }
        uint64_t mdia_end = mdia.offset + mdia.size;

        /* hdlr 的 handler_type 在 FullBox(4) + pre_defined(4) 之后 */
        mp4_box_t hdlr = {0};
        uint8_t h[12];
        if (mp4_box_find(fp, mdia.offset, mdia_end,
                         mp4_box_type('h', 'd', 'l', 'r'), &hdlr) != ESP_OK ||
            hdlr.size < 12 ||
            fseek(fp, (long)hdlr.offset, SEEK_SET) != 0 ||
            fread(h, 1, 12, fp) != 12 ||
            memcmp(h + 8, "soun", 4) != 0) {
            continue;   /* 视频轨/提示轨 */
        }

        /* mdhd：version 0 的 timescale 在 +12，version 1 在 +20 */
        mp4_box_t mdhd = {0};
        uint8_t m[32];
        if (mp4_box_find(fp, mdia.offset, mdia_end,
                         mp4_box_type('m', 'd', 'h', 'd'), &mdhd) == ESP_OK &&
            mdhd.size >= 20 &&
            fseek(fp, (long)mdhd.offset, SEEK_SET) == 0 &&
            fread(m, 1, 20, fp) == 20) {
            if (m[0] == 1 && mdhd.size >= 32 &&
                fread(m + 20, 1, 12, fp) == 12) {
                /* v1：前 20 字节已读入 m[0..19]，后续 12 字节落在 m[20..31]；
                   timescale 在 body+20，duration 在 body+24（8 字节） */
                t->timescale = mp4_be32(m + 20);
                t->duration_ts = (uint32_t)mp4_be64(m + 24);
            } else if (m[0] == 0) {
                t->timescale = mp4_be32(m + 12);
                t->duration_ts = mp4_be32(m + 16);
            }
        }
        if (t->timescale == 0) {
            continue;
        }

        mp4_box_t minf = {0};
        if (mp4_box_find(fp, mdia.offset, mdia_end,
                         mp4_box_type('m', 'i', 'n', 'f'), &minf) != ESP_OK) {
            continue;
        }
        mp4_box_t stbl = {0};
        if (mp4_box_find(fp, minf.offset, minf.offset + minf.size,
                         mp4_box_type('s', 't', 'b', 'l'), &stbl) != ESP_OK) {
            continue;
        }
        esp_err_t stbl_err = mp4_read_stbl(fp, t, stbl, &sr, &ch, &aot, err, err_len);
        if (stbl_err == ESP_OK) {
            found = true;
            break;
        }
        /* 该 trak 不是可播的 AAC-LC：清掉半成品，继续找下一条音轨 */
        mp4_demux_close(t);
        t = mp4_psram_calloc(1, sizeof(*t));
        if (t == NULL) {
            mp4_set_err(err, err_len, "MP4 轨道句柄重分配失败");
            return ESP_ERR_NO_MEM;
        }
        t->fp = fp;
        t->mdat_offset = mdat.offset;
        t->mdat_size = mdat.size;
    }

    if (!found) {
        if (err != NULL && err_len > 0 && err[0] == '\0') {
            mp4_set_err(err, err_len, "未找到可播放的 AAC-LC 音轨");
        }
        free(t);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 校验 sample 偏移落在 mdat 内（宽松：允许 sample 表指向文件内其它位置，
       但必须在本文件范围内，避免读到标签/目录之外） */
    uint64_t first = mp4_chunk_offset(t, 0);
    uint32_t first_size = mp4_sample_size(t, 0);
    if (first_size == 0 || first + first_size > file_size) {
        mp4_set_err(err, err_len, "首个 AAC sample 偏移越界");
        mp4_demux_close(t);
        return ESP_ERR_INVALID_SIZE;
    }
    mp4_cursor_reset(t);
    t->cur_sample = 0;

    ESP_LOGI(TAG, "MP4 opened: %uHz %uch AOT=%u, %u samples, %u chunks, asc=%u B",
             (unsigned)sr, (unsigned)ch, (unsigned)aot,
             (unsigned)t->sample_count, (unsigned)t->chunk_count,
             (unsigned)t->asc_len);
    *out = t;
    return ESP_OK;
}

esp_err_t mp4_demux_read_frame(mp4_track_t *t, uint8_t *buf, size_t cap, size_t *len)
{
    if (t == NULL || buf == NULL || len == NULL || t->fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *len = 0;
    if (t->cur_sample >= t->sample_count) {
        return ESP_ERR_NOT_FOUND;   /* 自然结束 */
    }
    uint64_t offset = 0;
    uint32_t size = 0;
    esp_err_t err = mp4_locate_sample(t, t->cur_sample, &offset, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size == 0 || size > cap) {
        return ESP_ERR_INVALID_SIZE;   /* 单帧超过调用方缓冲，明确报错不裁切 */
    }
    if (fseek(t->fp, (long)offset, SEEK_SET) != 0 ||
        fread(buf, 1, size, t->fp) != size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    t->cur_chunk_offset = offset + size;
    t->cur_sample++;
    *len = size;
    return ESP_OK;
}

esp_err_t mp4_demux_seek_sample(mp4_track_t *t, uint32_t sample_idx)
{
    if (t == NULL || sample_idx >= t->sample_count) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mp4_cursor_seek(t, sample_idx);
    if (err != ESP_OK) {
        return err;
    }
    t->cur_sample = sample_idx;
    return ESP_OK;
}

esp_err_t mp4_demux_sample_at_seconds(const mp4_track_t *t, uint32_t seconds,
                                      uint32_t *sample_idx)
{
    if (t == NULL || sample_idx == NULL || t->timescale == 0 || t->sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t target = (uint64_t)seconds * t->timescale;
    uint64_t acc_samples = 0;
    uint64_t acc_time = 0;
    for (uint32_t i = 0; i < t->stts_count; i++) {
        uint64_t count = t->stts[i].sample_count;
        uint64_t delta = t->stts[i].sample_delta;
        if (delta == 0) {
            continue;
        }
        uint64_t entry_time = count * delta;
        if (target < acc_time + entry_time) {
            uint64_t within = (target - acc_time) / delta;
            uint64_t idx = acc_samples + within;
            if (idx >= t->sample_count) {
                idx = t->sample_count - 1;
            }
            *sample_idx = (uint32_t)idx;
            return ESP_OK;
        }
        acc_samples += count;
        acc_time += entry_time;
    }
    *sample_idx = t->sample_count - 1;   /* 超过时长：停在最后一帧 */
    return ESP_OK;
}

esp_err_t mp4_demux_audio_config(const mp4_track_t *t, uint32_t *sample_rate,
                                 uint8_t *channels, uint8_t *aot)
{
    if (t == NULL || t->asc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t sr = 0;
    uint8_t ch = 0;
    uint8_t obj = 0;
    char dummy_err[64] = "";
    esp_err_t err = mp4_parse_asc(t->asc, t->asc_len, &sr, &ch, &obj,
                                  dummy_err, sizeof(dummy_err));
    if (err != ESP_OK) {
        return err;
    }
    if (sample_rate) {
        *sample_rate = sr;
    }
    if (channels) {
        *channels = ch;
    }
    if (aot) {
        *aot = obj;
    }
    return ESP_OK;
}

esp_err_t mp4_demux_find_cover(FILE *fp, uint8_t **jpeg, uint32_t *len)
{
    if (fp == NULL || jpeg == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *jpeg = NULL;
    *len = 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    long fsz = ftell(fp);
    if (fsz < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t file_size = (uint64_t)fsz;
    mp4_box_t moov = {0};
    if (mp4_box_find(fp, 0, file_size, mp4_box_type('m', 'o', 'o', 'v'), &moov) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    mp4_box_t udta = {0};
    if (mp4_box_find(fp, moov.offset, moov.offset + moov.size,
                     mp4_box_type('u', 'd', 't', 'a'), &udta) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    mp4_box_t meta = {0};
    if (mp4_box_find(fp, udta.offset, udta.offset + udta.size,
                     mp4_box_type('m', 'e', 't', 'a'), &meta) != ESP_OK ||
        meta.size < 4) {
        return ESP_ERR_NOT_FOUND;
    }
    /* meta 是 FullBox：前 4 字节 version/flags，子 box 从 +4 开始 */
    mp4_box_t ilst = {0};
    if (mp4_box_find(fp, meta.offset + 4, meta.offset + meta.size,
                     mp4_box_type('i', 'l', 's', 't'), &ilst) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    mp4_box_t covr = {0};
    if (mp4_box_find(fp, ilst.offset, ilst.offset + ilst.size,
                     mp4_box_type('c', 'o', 'v', 'r'), &covr) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    mp4_box_t data = {0};
    if (mp4_box_find(fp, covr.offset, covr.offset + covr.size,
                     mp4_box_type('d', 'a', 't', 'a'), &data) != ESP_OK ||
        data.size <= 8 || data.size - 8 > MP4_MAX_COVER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = mp4_psram_calloc(1, (size_t)(data.size - 8));
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* data box：4 字节 FullBox + 4 字节类型标识，其后是图片本体 */
    if (fseek(fp, (long)(data.offset + 8), SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)(data.size - 8), fp) != data.size - 8) {
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *jpeg = buf;
    *len = (uint32_t)(data.size - 8);
    return ESP_OK;
}

void mp4_demux_close(mp4_track_t *t)
{
    if (t == NULL) {
        return;
    }
    free(t->stsz);
    free(t->stco);
    free(t->co64);
    free(t->stsc);
    free(t->stts);
    free(t->asc);
    free(t);
}
