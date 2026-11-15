/**
 * WAV（RIFF/WAVE）流式解码 —— 本工程唯一自己实现的音频格式
 *
 * 为什么单开一个文件：MP3/FLAC/OGG 的解码都在第三方库里（minimp3 / dr_flac /
 * Tremor），audio_decoder.c 只做"分发 + 统一接口"；WAV 的 RIFF 解析与位宽
 * 转换是本项目自己的代码（约 300 行），放在分发器里会让它膨胀并混淆职责。
 * 这里对外只暴露 wav_decoder_* 接口，由 audio_decoder.c 调用。
 *
 * 格式覆盖与取舍见 wav_decoder.h 的头注释。
 */

#include "core/audio_decoders/wav_decoder.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "wav_dec";

/* fmt chunk 的 format tag */
#define WAV_FMT_PCM         0x0001
#define WAV_FMT_FLOAT       0x0003
#define WAV_FMT_EXTENSIBLE  0xFFFE

/* KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT 的 GUID 后半段（fmt+26 起 14 字节固定）：
   GUID 的 Data1 低 16 位是 1(PCM)/3(float)（在 fmt+24），其余部分两种子格式
   完全相同。用它可以挡住 EXTENSIBLE 里的压缩子格式（ADPCM 等 GUID 不同）。 */
static const uint8_t wav_guid_tail[14] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
};

/* 归一化后的采样编码类别 */
typedef enum {
    WAV_SAMPLE_PCM = 0,     /* 整数 PCM：8bit 无符号，16/24/32bit 有符号 */
    WAV_SAMPLE_FLOAT,       /* IEEE float32 */
} wav_sample_kind_t;

struct wav_decoder {
    FILE    *fp;
    uint64_t data_offset;      /* data chunk 音频数据的文件偏移 */
    uint64_t data_size;        /* 可读字节数（已按实际文件长度裁剪） */
    uint64_t data_pos;         /* 已读出的 data 字节数（用于在 data 末尾停住） */
    uint64_t total_frames;     /* data_size / block_align */
    uint16_t block_align;      /* 每帧字节数 = 声道数 × 每样本字节数 */
    uint16_t bits;             /* 容器位宽 8/16/24/32 */
    uint8_t  channels;
    uint32_t sample_rate;
    wav_sample_kind_t kind;
    uint8_t *raw;              /* 原始帧字节缓冲（按需增长，close 释放） */
    size_t   raw_cap;
};

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void wav_set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static uint16_t wav_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t wav_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 解析 fmt 块；成功返回 true 并填好采样参数，失败返回 false 并写 err */
static bool wav_parse_fmt(const uint8_t *fmt, uint32_t len, wav_decoder_t *w,
                          char *err, size_t err_len)
{
    uint16_t tag = wav_le16(fmt);
    uint32_t nch = wav_le16(fmt + 2);
    uint32_t sr = wav_le32(fmt + 4);
    uint32_t align = wav_le16(fmt + 12);
    uint32_t bits = wav_le16(fmt + 14);

    w->kind = WAV_SAMPLE_PCM;

    if (tag == WAV_FMT_EXTENSIBLE) {
        if (len < 40) {
            wav_set_err(err, err_len, "WAVE_FORMAT_EXTENSIBLE 的 fmt 块过短");
            return false;
        }
        uint16_t sub = wav_le16(fmt + 24);
        if (memcmp(fmt + 26, wav_guid_tail, sizeof(wav_guid_tail)) != 0) {
            wav_set_err(err, err_len, "EXTENSIBLE 子格式不是 PCM/float");
            return false;
        }
        tag = sub;
        /* valid_bits 不用单独处理：整数采样一律 MSB 对齐，取容器高 16bit
           就等价于按位宽归一化（24-in-32 与 24bit 文件得到同样的结果） */
    }

    if (tag == WAV_FMT_FLOAT) {
        if (bits != 32) {
            wav_set_err(err, err_len, "只支持 32bit float WAV（当前 %ubit）",
                        (unsigned)bits);
            return false;
        }
        w->kind = WAV_SAMPLE_FLOAT;
    } else if (tag == WAV_FMT_PCM) {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
            wav_set_err(err, err_len, "不支持的 PCM 位宽 %ubit（支持 8/16/24/32）",
                        (unsigned)bits);
            return false;
        }
    } else {
        /* 0x0002=MS ADPCM、0x0011=IMA ADPCM、0x0006=A-law、0x0007=mu-law ... */
        wav_set_err(err, err_len,
                    "不支持的 WAV 编码 0x%04x（仅 PCM / IEEE float）",
                    (unsigned)tag);
        return false;
    }

    if (nch != 1 && nch != 2) {
        wav_set_err(err, err_len, "不支持的声道数 %u（仅 1/2）", (unsigned)nch);
        return false;
    }
    if (sr < 8000 || sr > 192000) {
        wav_set_err(err, err_len, "WAV 采样率异常：%u", (unsigned)sr);
        return false;
    }

    w->bits = (uint16_t)bits;
    w->channels = (uint8_t)nch;
    w->sample_rate = sr;
    w->block_align = (uint16_t)(nch * (bits / 8));
    /* 极少数写入端会把 block_align 写错；以 fmt 推算值为准，避免整轨错位 */
    if (align != 0 && align != w->block_align) {
        ESP_LOGW(TAG, "block_align=%u 与 fmt 推算的 %u 不一致，按推算值处理",
                 (unsigned)align, (unsigned)w->block_align);
    }
    return true;
}

/* 扫描 RIFF 块，定位 fmt 与 data（两者都可能出现在任意位置，ID3/LIST 等跳过） */
static bool wav_scan_chunks(wav_decoder_t *w, char *err, size_t err_len)
{
    if (fseek(w->fp, 0, SEEK_END) != 0) {
        wav_set_err(err, err_len, "WAV 读取文件长度失败");
        return false;
    }
    long fsize = ftell(w->fp);
    if (fsize < 12) {
        wav_set_err(err, err_len, "WAV 文件过小");
        return false;
    }
    uint64_t file_size = (uint64_t)fsize;

    uint8_t hdr[12];
    if (fseek(w->fp, 0, SEEK_SET) != 0 ||
        fread(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr)) {
        wav_set_err(err, err_len, "WAV 读取文件头失败");
        return false;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 && memcmp(hdr, "RF64", 4) != 0) {
        wav_set_err(err, err_len, "不是 RIFF/WAVE 文件（RIFX 大端不支持）");
        return false;
    }
    if (memcmp(hdr + 8, "WAVE", 4) != 0) {
        wav_set_err(err, err_len, "RIFF 缺少 WAVE 标识");
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint64_t pos = 12;
    while (pos + 8 <= file_size) {
        if (fseek(w->fp, (long)pos, SEEK_SET) != 0) {
            break;
        }
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), w->fp) != sizeof(ch)) {
            break;
        }
        uint32_t sz = wav_le32(ch + 4);
        uint64_t body = pos + 8;
        uint64_t remain = file_size - body;

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[64];
            uint32_t n = (sz > sizeof(fmt)) ? (uint32_t)sizeof(fmt) : sz;
            if (n < 16) {
                wav_set_err(err, err_len, "WAV fmt 块过短");
                return false;
            }
            if (fread(fmt, 1, n, w->fp) != n) {
                break;
            }
            if (!wav_parse_fmt(fmt, n, w, err, err_len)) {
                return false;   /* parse 已写好原因 */
            }
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_offset = body;
            /* 0 / 0xFFFFFFFF 都表示"长度未知"（RF64 或写入端没回填）：
               用文件剩余长度兜底，总比读 0 帧好 */
            if (sz == 0 || sz == 0xFFFFFFFFu || (uint64_t)sz > remain) {
                w->data_size = remain;
            } else {
                w->data_size = (uint64_t)sz;
            }
            have_data = true;
        }

        if (have_fmt && have_data) {
            break;   /* 已经拿全，后面的 ID3/LIST 与播放无关 */
        }

        /* 块长度是奇数时有一个填充字节；长度异常时停止，防止死循环 */
        uint64_t next = body + (uint64_t)sz + (sz & 1u);
        if (next <= pos) {
            break;
        }
        pos = next;
    }

    if (!have_fmt) {
        wav_set_err(err, err_len, "WAV 缺少 fmt 块");
        return false;
    }
    if (!have_data) {
        wav_set_err(err, err_len, "WAV 缺少 data 块");
        return false;
    }
    return true;
}

/* 按需扩容原始帧缓冲（每帧 block_align 字节） */
static bool wav_raw_ensure(wav_decoder_t *w, size_t need)
{
    if (need <= w->raw_cap) {
        return true;
    }
    uint8_t *p = realloc(w->raw, need);
    if (p == NULL) {
        return false;
    }
    w->raw = p;
    w->raw_cap = need;
    return true;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

wav_decoder_t *wav_decoder_open(const char *path, char *err, size_t err_len)
{
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (path == NULL) {
        wav_set_err(err, err_len, "路径为空");
        return NULL;
    }

    wav_decoder_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        wav_set_err(err, err_len, "内存不足");
        return NULL;
    }

    w->fp = fopen(path, "rb");
    if (w->fp == NULL) {
        wav_set_err(err, err_len, "无法打开文件");
        free(w);
        return NULL;
    }
    /* 关掉 stdio 缓冲：每次读取就是 512 帧（2~4KB），newlib 的懒分配缓冲只会
       多一次拷贝，还要占内部 RAM */
    setvbuf(w->fp, NULL, _IONBF, 0);

    if (!wav_scan_chunks(w, err, err_len)) {
        fclose(w->fp);
        free(w);
        return NULL;
    }

    w->total_frames = (w->block_align != 0) ? w->data_size / w->block_align : 0;

    /* 定位到音频数据起点；seek 也基于这个偏移 */
    if (fseek(w->fp, (long)w->data_offset, SEEK_SET) != 0) {
        wav_set_err(err, err_len, "WAV 定位 data 块失败");
        fclose(w->fp);
        free(w);
        return NULL;
    }

    ESP_LOGI(TAG, "WAV opened: %s %uHz %uch %ubit %s, %llu frames",
             path, (unsigned)w->sample_rate, (unsigned)w->channels,
             (unsigned)w->bits,
             w->kind == WAV_SAMPLE_FLOAT ? "float" : "pcm",
             (unsigned long long)w->total_frames);
    return w;
}

int wav_decoder_read(wav_decoder_t *w, int16_t *out, int max_frames)
{
    if (w == NULL || out == NULL || max_frames <= 0 || w->fp == NULL) {
        return -1;
    }

    /* ⚠ 必须按 data_size 截断：WAV 的 data 后面常跟 ID3/LIST 等 chunk
       （本项目的样本就是 data 之后 718KB 的 ID3），只靠 EOF 判断会把
       标签数据当成 PCM 播出来 —— 表现为"整曲播完后又放几秒满幅白噪音"。
       data 长度未知时（写 0 / RF64）data_size 已按文件剩余长度兜底。 */
    if (w->data_pos >= w->data_size) {
        return 0;   /* data 读完 = 自然结束 */
    }
    size_t need = (size_t)max_frames * w->block_align;
    uint64_t remain = w->data_size - w->data_pos;
    if ((uint64_t)need > remain) {
        need = (size_t)remain;
    }

    if (!wav_raw_ensure(w, need)) {
        return -1;
    }
    size_t got = fread(w->raw, 1, need, w->fp);
    int frames = (int)(got / w->block_align);   /* 丢掉不完整的一帧 */
    w->data_pos += got;
    if (frames <= 0) {
        return 0;   /* 正常播完 */
    }

    /* 统一转成交织 16bit：整数取容器高 16bit（MSB 对齐），float 裁剪后乘 32767 */
    const uint8_t *p = w->raw;
    int n = frames * (int)w->channels;
    if (w->kind == WAV_SAMPLE_FLOAT) {
        for (int i = 0; i < n; i++, p += 4) {
            float f;
            memcpy(&f, p, sizeof(f));
            if (f > 1.0f) {
                f = 1.0f;
            } else if (f < -1.0f) {
                f = -1.0f;
            }
            out[i] = (int16_t)(f * 32767.0f);
        }
    } else if (w->bits == 8) {
        /* 8bit WAV 是无符号（0..255，中点 128） */
        for (int i = 0; i < n; i++, p++) {
            out[i] = (int16_t)(((int)p[0] - 128) << 8);
        }
    } else if (w->bits == 16) {
        for (int i = 0; i < n; i++, p += 2) {
            out[i] = (int16_t)wav_le16(p);
        }
    } else if (w->bits == 24) {
        for (int i = 0; i < n; i++, p += 3) {
            int32_t v = (int32_t)((uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16));
            if (v & 0x800000) {
                v -= 0x1000000;   /* 24bit 符号扩展 */
            }
            out[i] = (int16_t)(v >> 8);
        }
    } else {   /* 32bit 整数 */
        for (int i = 0; i < n; i++, p += 4) {
            out[i] = (int16_t)((int32_t)wav_le32(p) >> 16);
        }
    }
    return frames;
}

esp_err_t wav_decoder_seek(wav_decoder_t *w, uint32_t seconds)
{
    if (w == NULL || w->fp == NULL || w->sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* WAV 是定长帧：直接按样本偏移算字节位置，不需要建索引 */
    uint64_t off = (uint64_t)seconds * w->sample_rate * w->block_align;
    if (off > w->data_size) {
        off = w->data_size;
    }
    if (fseek(w->fp, (long)(w->data_offset + off), SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    w->data_pos = off;   /* 与文件位置同步，read 才能正确截断 */
    return ESP_OK;
}

uint32_t wav_decoder_sample_rate(const wav_decoder_t *w)
{
    return w ? w->sample_rate : 0;
}

uint32_t wav_decoder_channels(const wav_decoder_t *w)
{
    return w ? w->channels : 0;
}

uint64_t wav_decoder_total_frames(const wav_decoder_t *w)
{
    return w ? w->total_frames : 0;
}

void wav_decoder_close(wav_decoder_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w->fp != NULL) {
        fclose(w->fp);
    }
    free(w->raw);
    free(w);
}
