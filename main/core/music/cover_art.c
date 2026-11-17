#include "core/music/cover_art.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#if CONFIG_AUDIO_ENABLE_AAC
#include "core/audio_decoders/mp4_demux.h"
#endif

static const char *TAG = "cover_art";

/* 单张封面上限：常见 1000x1000 JPEG 约 100~500KB，2MB 足够且不会让
   PSRAM 峰值过高。 */
#define CA_MAX_JPEG          (2u * 1024u * 1024u)
/* ID3 标签/FLAC 图片块上限（含 padding 与其它帧） */
#define CA_MAX_TAG           (4u * 1024u * 1024u)
#define CA_MAX_PICTURE_BLOCK (CA_MAX_JPEG + 4096u)
/* OGG 注释包（含 base64 图片）上限 */
#define CA_MAX_OGG_COMMENT   (4u * 1024u * 1024u)
/* OGG 单页最大正文：255 个 segment × 255 字节 */
#define CA_OGG_PAGE_BODY_MAX (255u * 255u)
#define CA_PATH_MAX          512

static void *ca_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static uint32_t ca_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t ca_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ID3v2 的 synchsafe 整数：每字节只用低 7 位 */
static uint32_t ca_synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) |
           ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) |
           (uint32_t)(p[3] & 0x7F);
}

static bool ca_is_jpeg(const uint8_t *p, size_t len)
{
    return p != NULL && len >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

/* 复制封面数据到独立缓冲（PSRAM 优先），并校验 JPEG 文件头 */
static uint8_t *ca_copy_jpeg(const uint8_t *data, size_t len)
{
    if (!ca_is_jpeg(data, len) || len > CA_MAX_JPEG) {
        return NULL;
    }
    uint8_t *out = ca_alloc(len);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, data, len);
    return out;
}

/* ------------------------------------------------------------------ */
/* MP3 / ID3v2：PIC(v2.2) / APIC(v2.3/2.4)                            */
/* ------------------------------------------------------------------ */

/* 跳过 ID3 文本描述（含结尾 0）。UTF-16 用双字节 0，其余编码用单字节 0 */
static const uint8_t *ca_skip_id3_string(const uint8_t *p, const uint8_t *end,
                                         uint8_t encoding)
{
    if (encoding == 1 || encoding == 2) {   /* UTF-16 with BOM / UTF-16BE */
        while (p + 1 < end) {
            if (p[0] == 0 && p[1] == 0) {
                return p + 2;
            }
            p += 2;
        }
        return end;
    }
    const uint8_t *z = memchr(p, 0, (size_t)(end - p));
    return z ? z + 1 : end;
}

/* 解析 APIC/PIC 帧体，返回 JPEG 指针（指向 tag 内部；不复制） */
static const uint8_t *ca_apic_payload(const uint8_t *p, const uint8_t *end,
                                      uint8_t ver_major, uint32_t *jpeg_len,
                                      bool *is_front)
{
    if (p >= end) {
        return NULL;
    }
    uint8_t encoding = *p++;

    if (ver_major == 2) {
        /* v2.2 PIC：3 字节图片格式（"JPG"/"PNG"），随后 picture type */
        if (end - p < 4) {
            return NULL;
        }
        p += 3;
    } else {
        /* v2.3/2.4 APIC：MIME 字符串（latin1，0 结尾） */
        const uint8_t *z = memchr(p, 0, (size_t)(end - p));
        if (z == NULL) {
            return NULL;
        }
        p = z + 1;
    }

    if (p >= end) {
        return NULL;
    }
    uint8_t pic_type = *p++;
    p = ca_skip_id3_string(p, end, encoding);
    if (p >= end) {
        return NULL;
    }

    size_t rem = (size_t)(end - p);
    if (!ca_is_jpeg(p, rem) || rem > CA_MAX_JPEG) {
        return NULL;
    }
    *jpeg_len = (uint32_t)rem;
    *is_front = (pic_type == 3);   /* front cover */
    return p;
}

/* 解析一段完整的 ID3v2 标签（tag 指向 "ID3" 头），找 APIC/PIC 里的 JPEG。
   MP3 的文件头标签与 WAV 的 "ID3 " chunk 共用这个函数（chunk 里就是完整的
   ID3 标签，所以 WAV 只是"换个位置找到标签"）。
   成功时返回新分配的 JPEG 缓冲（best/front 规则：front 优先）。 */
static esp_err_t ca_id3_parse(const uint8_t *tag, size_t total,
                              uint8_t **out, uint32_t *out_len)
{
    if (total < 10 || memcmp(tag, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t ver_major = tag[3];
    uint8_t flags = tag[5];
    uint32_t body = ca_synchsafe32(tag + 6);
    /* 标签体必须整段落在给定缓冲里（WAV 的 chunk 长度可能与标签自报值略有出入） */
    if (body == 0 || (size_t)body > total - 10u) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *p = tag + 10;
    const uint8_t *end = tag + 10 + body;   /* 不含 footer */

    /* 扩展头：v2.3 的长度不含自身，v2.4 含自身 */
    if (flags & 0x40) {
        if (end - p < 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t ext = (ver_major >= 4) ? ca_synchsafe32(p) : ca_be32(p);
        size_t skip = (ver_major >= 4) ? ext : (size_t)ext + 4u;
        if (skip > (size_t)(end - p)) {
            return ESP_ERR_INVALID_SIZE;
        }
        p += skip;
    }

    uint8_t *best = NULL;
    uint32_t best_len = 0;

    while (p < end) {
        const uint8_t *id = p;
        uint32_t size;
        size_t hdr_len;

        if (ver_major == 2) {
            if (end - p < 6) {
                break;
            }
            size = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
            hdr_len = 6;
        } else {
            if (end - p < 10) {
                break;
            }
            size = (ver_major >= 4) ? ca_synchsafe32(p + 4) : ca_be32(p + 4);
            hdr_len = 10;
        }

        /* 全 0 表示进入 padding */
        if (id[0] == 0 || size == 0) {
            break;
        }
        if ((size_t)size > (size_t)(end - (p + hdr_len))) {
            break;
        }

        bool is_apic = (ver_major == 2)
                           ? (memcmp(id, "PIC", 3) == 0)
                           : (memcmp(id, "APIC", 4) == 0);
        if (is_apic) {
            uint32_t jlen = 0;
            bool front = false;
            const uint8_t *jpg = ca_apic_payload(p + hdr_len,
                                                 p + hdr_len + size,
                                                 ver_major, &jlen, &front);
            if (jpg != NULL) {
                uint8_t *copy = ca_copy_jpeg(jpg, jlen);
                if (copy != NULL && front) {
                    free(best);
                    *out = copy;
                    *out_len = jlen;
                    return ESP_OK;
                }
                if (copy != NULL && best == NULL) {
                    best = copy;
                    best_len = jlen;
                } else {
                    free(copy);
                }
            }
        }

        p += hdr_len + size;
    }

    if (best != NULL) {
        *out = best;
        *out_len = best_len;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* 读取 MP3 文件头的 ID3 标签并找 APIC。成功时返回新分配的 JPEG 缓冲 */
static esp_err_t ca_id3_find(const char *path, uint8_t **out, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "ID3", 3) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t flags = hdr[5];
    uint32_t body = ca_synchsafe32(hdr + 6);
    /* v2.4 footer 标志：标签末尾额外 10 字节 */
    size_t total = 10u + (size_t)body + ((flags & 0x10) ? 10u : 0u);
    if (body == 0 || total > CA_MAX_TAG) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *tag = ca_alloc(total);
    if (tag == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fseek(f, 0, SEEK_SET) != 0 ||
        fread(tag, 1, total, f) != total) {
        free(tag);
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    fclose(f);

    esp_err_t ret = ca_id3_parse(tag, total, out, out_len);
    free(tag);
    return ret;
}

/* ------------------------------------------------------------------ */
/* WAV：RIFF 里的 "ID3 " chunk（ID3v2 标签被塞进 WAV 容器）            */
/* ------------------------------------------------------------------ */

/* 扫描 RIFF 块找 "ID3 "（或小写 "id3 "，少见）chunk，交给 ca_id3_parse。
   ⚠ 只认 JPEG：内嵌 PNG 封面会被 ca_apic_payload 的 JPEG 头校验跳过，
     回退到同目录封面/默认封面（本工程不做 PNG 解码）。 */
static esp_err_t ca_wav_find(const char *path, uint8_t **out,
                             uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        (memcmp(hdr, "RIFF", 4) != 0 && memcmp(hdr, "RF64", 4) != 0) ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    long fsize = ftell(f);
    long pos = 12;
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    while (fsize > 0 && pos + 8 <= fsize) {
        uint8_t ch[8];
        if (fseek(f, pos, SEEK_SET) != 0 ||
            fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
            break;
        }
        uint32_t sz = ca_le32(ch + 4);
        long body = pos + 8;

        if (memcmp(ch, "ID3 ", 4) == 0 || memcmp(ch, "id3 ", 4) == 0) {
            if (sz == 0 || sz > CA_MAX_TAG || (long)sz > fsize - body) {
                break;
            }
            uint8_t *tag = ca_alloc(sz);
            if (tag == NULL) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            if (fread(tag, 1, sz, f) != sz) {
                free(tag);
                ret = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            ESP_LOGD(TAG, "WAV ID3 chunk: %u B", (unsigned)sz);
            ret = ca_id3_parse(tag, sz, out, out_len);
            free(tag);
            if (ret != ESP_OK) {
                /* 很多 WAV 的内嵌封面是 PNG，而本工程只解 JPEG；明确提示一次，
                   免得用户以为封面解析没生效（还会继续回退同目录封面） */
                ESP_LOGI(TAG, "WAV ID3 chunk 里没有 JPEG 封面"
                              "（PNG 等其它格式不支持，回退目录封面）");
            }
            break;   /* 只处理第一个 ID3 chunk */
        }

        /* 块长度是奇数时有一个填充字节；长度异常时停止，避免死循环 */
        long next = body + (long)sz + (long)(sz & 1u);
        if (next <= pos) {
            break;
        }
        pos = next;
    }

    fclose(f);
    return ret;
}

/* ------------------------------------------------------------------ */
/* FLAC：PICTURE 元数据块                                              */
/* ------------------------------------------------------------------ */

/* 解析 FLAC PICTURE 块（也用于 OGG 的 METADATA_BLOCK_PICTURE）。
   返回新分配的 JPEG 缓冲；is_front 表示 picture type == 3。 */
static uint8_t *ca_flac_picture(const uint8_t *p, size_t len,
                                uint32_t *jpeg_len, bool *is_front)
{
    const uint8_t *end = p + len;
    if (len < 32) {
        return NULL;
    }

    uint32_t pic_type = ca_be32(p);
    p += 4;
    uint32_t mime_len = ca_be32(p);
    p += 4;
    if (mime_len > (uint32_t)(end - p)) {
        return NULL;
    }
    p += mime_len;

    if (end - p < 4) {
        return NULL;
    }
    uint32_t desc_len = ca_be32(p);
    p += 4;
    if (desc_len > (uint32_t)(end - p)) {
        return NULL;
    }
    p += desc_len;

    /* width/height/depth/colors：4×uint32 */
    if (end - p < 16 + 4) {
        return NULL;
    }
    p += 16;
    uint32_t data_len = ca_be32(p);
    p += 4;
    if (data_len > (uint32_t)(end - p) || data_len == 0 ||
        data_len > CA_MAX_JPEG) {
        return NULL;
    }

    if (!ca_is_jpeg(p, data_len)) {
        return NULL;
    }
    uint8_t *copy = ca_copy_jpeg(p, data_len);
    if (copy != NULL) {
        *jpeg_len = data_len;
        *is_front = (pic_type == 3);
    }
    return copy;
}

/* 跳过可能存在的 ID3v2 前缀（有些 FLAC 文件带 ID3 标签） */
static bool ca_skip_id3_prefix(FILE *f)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return false;
    }
    uint32_t body = ca_synchsafe32(hdr + 6);
    size_t skip = 10u + (size_t)body + ((hdr[5] & 0x10) ? 10u : 0u);
    fseek(f, (long)skip, SEEK_SET);
    return true;
}

static esp_err_t ca_flac_find(const char *path, uint8_t **out, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ca_skip_id3_prefix(f);

    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, "fLaC", 4) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *best = NULL;
    uint32_t best_len = 0;

    while (true) {
        uint8_t bh[4];
        if (fread(bh, 1, sizeof(bh), f) != sizeof(bh)) {
            break;
        }
        bool last = (bh[0] & 0x80) != 0;
        uint8_t type = bh[0] & 0x7F;
        uint32_t len = ((uint32_t)bh[1] << 16) |
                       ((uint32_t)bh[2] << 8) | bh[3];

        if (type == 6) {   /* PICTURE */
            if (len > CA_MAX_PICTURE_BLOCK) {
                break;
            }
            uint8_t *block = ca_alloc(len);
            if (block == NULL) {
                fclose(f);
                free(best);
                return ESP_ERR_NO_MEM;
            }
            if (fread(block, 1, len, f) != len) {
                free(block);
                break;
            }
            uint32_t jlen = 0;
            bool front = false;
            uint8_t *jpg = ca_flac_picture(block, len, &jlen, &front);
            free(block);
            if (jpg != NULL) {
                if (front) {
                    free(best);
                    fclose(f);
                    *out = jpg;
                    *out_len = jlen;
                    return ESP_OK;
                }
                if (best == NULL) {
                    best = jpg;
                    best_len = jlen;
                } else {
                    free(jpg);
                }
            }
        } else if (fseek(f, (long)len, SEEK_CUR) != 0) {
            break;
        }

        if (last) {
            break;
        }
    }

    fclose(f);
    if (best != NULL) {
        *out = best;
        *out_len = best_len;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* OGG Vorbis：METADATA_BLOCK_PICTURE / COVERART                       */
/* ------------------------------------------------------------------ */

static int ca_b64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *ca_b64_decode(const uint8_t *src, size_t len, size_t *out_len)
{
    size_t cap = (len / 4 + 1) * 3 + 4;
    uint8_t *out = ca_alloc(cap);
    if (out == NULL) {
        return NULL;
    }

    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            break;
        }
        int v = ca_b64_value(c);
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

static bool ca_key_equals(const uint8_t *p, size_t len, const char *key)
{
    size_t klen = strlen(key);
    return len == klen && strncasecmp((const char *)p, key, klen) == 0;
}

/* 解析 Vorbis comment 包，找图片 */
static esp_err_t ca_vorbis_comment(const uint8_t *pkt, size_t len,
                                   uint8_t **out, uint32_t *out_len)
{
    if (len < 7 || pkt[0] != 3 || memcmp(pkt + 1, "vorbis", 6) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *p = pkt + 7;
    const uint8_t *end = pkt + len;

    if (end - p < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t vendor_len = ca_le32(p);
    p += 4;
    if (vendor_len > (uint32_t)(end - p)) {
        return ESP_ERR_INVALID_SIZE;
    }
    p += vendor_len;

    if (end - p < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t count = ca_le32(p);
    p += 4;

    ESP_LOGD(TAG, "vorbis comment: packet=%u B, %u comments",
             (unsigned)len, (unsigned)count);

    uint8_t *best = NULL;
    uint32_t best_len = 0;

    for (uint32_t i = 0; i < count && p + 4 <= end; i++) {
        uint32_t clen = ca_le32(p);
        p += 4;
        if (clen > (uint32_t)(end - p)) {
            break;
        }

        const uint8_t *eq = memchr(p, '=', clen);
        if (eq != NULL) {
            size_t key_len = (size_t)(eq - p);
            const uint8_t *val = eq + 1;
            size_t val_len = clen - key_len - 1;

            if (ca_key_equals(p, key_len, "METADATA_BLOCK_PICTURE")) {
                size_t dec_len = 0;
                uint8_t *dec = ca_b64_decode(val, val_len, &dec_len);
                if (dec != NULL) {
                    uint32_t jlen = 0;
                    bool front = false;
                    uint8_t *jpg = ca_flac_picture(dec, dec_len, &jlen, &front);
                    free(dec);
                    if (jpg != NULL) {
                        if (front) {
                            free(best);
                            *out = jpg;
                            *out_len = jlen;
                            return ESP_OK;
                        }
                        if (best == NULL) {
                            best = jpg;
                            best_len = jlen;
                        } else {
                            free(jpg);
                        }
                    }
                }
            } else if (ca_key_equals(p, key_len, "COVERART")) {
                size_t dec_len = 0;
                uint8_t *dec = ca_b64_decode(val, val_len, &dec_len);
                if (dec != NULL) {
                    if (ca_is_jpeg(dec, dec_len) && dec_len <= CA_MAX_JPEG) {
                        if (best == NULL) {
                            best = dec;
                            best_len = (uint32_t)dec_len;
                            dec = NULL;
                        }
                    }
                    free(dec);
                }
            }
        }
        p += clen;
    }

    if (best != NULL) {
        *out = best;
        *out_len = best_len;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t max;
} ca_packet_t;

static bool ca_packet_append(ca_packet_t *pkt, const uint8_t *data, size_t len)
{
    if (pkt->len + len > pkt->max) {
        return false;
    }
    if (pkt->len + len > pkt->cap) {
        size_t cap = pkt->cap ? pkt->cap : 1024;
        while (cap < pkt->len + len) {
            cap *= 2;
        }
        if (cap > pkt->max) {
            cap = pkt->max;
        }
        uint8_t *nb = ca_alloc(cap);
        if (nb == NULL) {
            return false;
        }
        if (pkt->buf != NULL) {
            memcpy(nb, pkt->buf, pkt->len);
            free(pkt->buf);
        }
        pkt->buf = nb;
        pkt->cap = cap;
    }
    memcpy(pkt->buf + pkt->len, data, len);
    pkt->len += len;
    return true;
}

static esp_err_t ca_ogg_find(const char *path, uint8_t **out, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *body = ca_alloc(CA_OGG_PAGE_BODY_MAX);
    if (body == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    ca_packet_t pkt = {
        .max = CA_MAX_OGG_COMMENT,
    };

    uint32_t stream_serial = 0;
    bool have_serial = false;
    int packet_index = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    while (packet_index < 2) {
        uint8_t ph[27];
        if (fread(ph, 1, sizeof(ph), f) != sizeof(ph) ||
            memcmp(ph, "OggS", 4) != 0) {
            break;
        }
        uint8_t nseg = ph[26];
        uint8_t lacing[255];
        if (fread(lacing, 1, nseg, f) != nseg) {
            break;
        }
        size_t body_len = 0;
        for (uint8_t i = 0; i < nseg; i++) {
            body_len += lacing[i];
        }
        if (body_len > CA_OGG_PAGE_BODY_MAX ||
            fread(body, 1, body_len, f) != body_len) {
            break;
        }

        uint32_t serial = ca_le32(ph + 14);
        if (!have_serial) {
            have_serial = true;
            stream_serial = serial;
        }
        if (serial != stream_serial) {
            continue;   /* 其它逻辑流（如封面流）暂不处理 */
        }

        size_t off = 0;
        for (uint8_t i = 0; i < nseg; i++) {
            uint8_t seg = lacing[i];
            if (!ca_packet_append(&pkt, body + off, seg)) {
                result = ESP_ERR_INVALID_SIZE;
                goto done;
            }
            off += seg;

            if (seg < 255) {   /* 包结束 */
                if (packet_index == 1) {
                    result = ca_vorbis_comment(pkt.buf, pkt.len, out, out_len);
                    goto done;
                }
                packet_index++;
                pkt.len = 0;
                if (packet_index >= 2) {
                    goto done;
                }
            }
        }
    }

done:
    free(pkt.buf);
    free(body);
    fclose(f);
    return result;
}

/* ------------------------------------------------------------------ */
/* 目录封面：cover/folder/front/album/封面 .jpg/.jpeg                  */
/* ------------------------------------------------------------------ */

static bool ca_has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);
    return n >= e && strcasecmp(name + n - e, ext) == 0;
}

static bool ca_has_jpeg_ext(const char *name)
{
    return ca_has_ext(name, ".jpg") || ca_has_ext(name, ".jpeg");
}

/* 名字优先级：0 最好；100 = 名字含 cover 等关键词；200 = 普通 jpg */
static int ca_cover_name_score(const char *name)
{
    static const char *const exact[] = {
        "cover", "folder", "front", "album", "albumart", "artwork", "封面",
    };

    char stem[256];
    size_t n = strlen(name);
    size_t i = 0;
    while (i < n && i + 1 < sizeof(stem) && name[i] != '.') {
        stem[i] = (char)tolower((unsigned char)name[i]);
        i++;
    }
    stem[i] = '\0';

    for (size_t k = 0; k < sizeof(exact) / sizeof(exact[0]); k++) {
        if (strcasecmp(stem, exact[k]) == 0) {
            return (int)k;
        }
    }

    static const char *const keyword[] = {
        "cover", "folder", "front", "album", "artwork",
    };
    for (size_t k = 0; k < sizeof(keyword) / sizeof(keyword[0]); k++) {
        if (strstr(stem, keyword[k]) != NULL) {
            return 100;
        }
    }
    return 200;
}

static esp_err_t ca_folder_find(const char *song_path, uint8_t **out,
                                uint32_t *out_len)
{
    const char *slash = strrchr(song_path, '/');
    if (slash == NULL || slash == song_path) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t dir_len = (size_t)(slash - song_path);
    if (dir_len >= CA_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char dir[CA_PATH_MAX];
    memcpy(dir, song_path, dir_len);
    dir[dir_len] = '\0';

    DIR *d = opendir(dir);
    if (d == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char best_name[300] = "";
    int best_score = 1000;
    int jpg_count = 0;
    int png_count = 0;
    char png_name[300] = "";

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (ca_has_ext(name, ".png")) {
            if (png_count == 0) {
                snprintf(png_name, sizeof(png_name), "%s", name);
            }
            png_count++;
        }
        if (!ca_has_jpeg_ext(name)) {
            continue;
        }
        jpg_count++;
        int score = ca_cover_name_score(name);
        if (score < best_score) {
            best_score = score;
            snprintf(best_name, sizeof(best_name), "%s", name);
            if (score == 0) {
                break;
            }
        }
    }
    closedir(d);
    ESP_LOGD(TAG, "folder scan: %s (%d jpg, %d png '%s', best '%s' score %d)",
             dir, jpg_count, png_count, png_name, best_name, best_score);
    if (png_count > 0 && jpg_count == 0) {
        ESP_LOGW(TAG, "目录里有 PNG 封面（%s）但当前只支持 JPEG，请转成 cover.jpg",
                 png_name);
    }

    if (best_name[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    /* 没有封面类文件名时，只有目录里恰好一张 jpg 才敢用，避免拿错图 */
    if (best_score >= 200 && jpg_count != 1) {
        return ESP_ERR_NOT_FOUND;
    }

    char img_path[CA_PATH_MAX];
    int n = snprintf(img_path, sizeof(img_path), "%s/%s", dir, best_name);
    if (n < 0 || (size_t)n >= sizeof(img_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(img_path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > CA_MAX_JPEG) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *buf = ca_alloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    fclose(f);

    if (!ca_is_jpeg(buf, (size_t)size)) {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "folder cover: %s (%ld B)", img_path, size);
    *out = buf;
    *out_len = (uint32_t)size;
    return ESP_OK;
}

#if CONFIG_AUDIO_ENABLE_AAC
/* ------------------------------------------------------------------ */
/* MP4/M4A：moov/udta/meta/ilst/covr                                  */
/* ------------------------------------------------------------------ */

/* 只取 JPEG 封面；PNG covr 明确打一行日志后回退同目录/默认图 */
static esp_err_t ca_mp4_find(const char *path, uint8_t **out, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *data = NULL;
    uint32_t len = 0;
    esp_err_t err = mp4_demux_find_cover(f, &data, &len);
    fclose(f);
    if (err != ESP_OK) {
        return err;
    }
    if (!ca_is_jpeg(data, len)) {
        ESP_LOGI(TAG, "m4a covr 不是 JPEG（PNG 等），走封面回退链");
        free(data);
        return ESP_ERR_NOT_FOUND;
    }
    if (len > CA_MAX_JPEG) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    *out = data;
    *out_len = len;
    return ESP_OK;
}
#endif /* CONFIG_AUDIO_ENABLE_AAC */

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t cover_art_find(const char *song_path, uint8_t **jpeg_out,
                         uint32_t *len_out)
{
    if (song_path == NULL || jpeg_out == NULL || len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *jpeg_out = NULL;
    *len_out = 0;

    const char *dot = strrchr(song_path, '.');
    esp_err_t err = ESP_ERR_NOT_FOUND;
    esp_err_t embed_err = ESP_ERR_NOT_FOUND;

    if (dot != NULL) {
        if (strcasecmp(dot, ".mp3") == 0) {
            err = ca_id3_find(song_path, jpeg_out, len_out);
            embed_err = err;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "embedded cover: MP3 ID3 APIC (%u B)",
                         (unsigned)*len_out);
                return ESP_OK;
            }
        } else if (strcasecmp(dot, ".flac") == 0) {
            err = ca_flac_find(song_path, jpeg_out, len_out);
            embed_err = err;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "embedded cover: FLAC PICTURE (%u B)",
                         (unsigned)*len_out);
                return ESP_OK;
            }
        } else if (strcasecmp(dot, ".ogg") == 0 ||
                   strcasecmp(dot, ".oga") == 0) {
            err = ca_ogg_find(song_path, jpeg_out, len_out);
            embed_err = err;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "embedded cover: OGG Vorbis comment (%u B)",
                         (unsigned)*len_out);
                return ESP_OK;
            }
        } else if (strcasecmp(dot, ".wav") == 0) {
            /* WAV 的封面惯例是塞一个 "ID3 " chunk（ID3v2 标签），少部分播放器
               写 LIST/INFO，但里面只有文字没有图片 */
            err = ca_wav_find(song_path, jpeg_out, len_out);
            embed_err = err;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "embedded cover: WAV ID3 chunk (%u B)",
                         (unsigned)*len_out);
                return ESP_OK;
            }
#if CONFIG_AUDIO_ENABLE_AAC
        } else if (strcasecmp(dot, ".m4a") == 0) {
            err = ca_mp4_find(song_path, jpeg_out, len_out);
            embed_err = err;
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "embedded cover: MP4 covr JPEG (%u B)",
                         (unsigned)*len_out);
                return ESP_OK;
            }
#endif
        }
    }

    err = ca_folder_find(song_path, jpeg_out, len_out);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no cover: %s (embed=%d folder=%d)",
             song_path, embed_err, err);
    return ESP_ERR_NOT_FOUND;
}

void cover_art_free(void *p)
{
    free(p);
}
