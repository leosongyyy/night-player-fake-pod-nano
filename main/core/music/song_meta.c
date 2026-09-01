#include "core/music/song_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "song_meta";

/* 文本字段长度上限（UTF-8 字节；标题/副标题 512 字节足够，防御异常文件） */
#define META_TEXT_MAX   512

/* JPEG 封面大小上限：460x460 JPEG 质量 85 一般 60~120KB，放宽到 256KB */
#define META_JPEG_MAX   (256 * 1024)

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */

/* 小端读取 uint32 */
static uint32_t meta_read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 复制 len 字节为 null 结尾字符串（PSRAM 优先，内部 RAM 兜底）。
   len 为 0 或超限返回 NULL（与"空字符串"区分：NULL 表示无此字段） */
static char *meta_copy_text(const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > META_TEXT_MAX) {
        return NULL;
    }
    char *s = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s == NULL) {
        s = malloc(len + 1);
    }
    if (s == NULL) {
        return NULL;
    }
    memcpy(s, data, len);
    s[len] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

size_t song_meta_build_npd_path(const char *song_path, char *buf, size_t buf_size)
{
    if (song_path == NULL || buf == NULL || buf_size == 0) {
        return 0;
    }
    /* "/sdcard/" 之后插入 ".ecov/"：
       "/sdcard/abc/x.mp3" → "/sdcard/.ecov/abc/x.mp3.npd" */
    const char *rel = strstr(song_path, "/sdcard/");
    if (rel == NULL) {
        return 0;
    }
    rel += strlen("/sdcard/");   /* 相对路径起点（目录/文件部分） */
    int n = snprintf(buf, buf_size, "/sdcard/.ecov/%s.npd", rel);
    if (n < 0 || (size_t)n >= buf_size) {
        return 0;
    }
    return (size_t)n;
}

esp_err_t song_meta_load(const char *song_path, song_meta_t *out)
{
    if (song_path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char npd_path[512];
    if (song_meta_build_npd_path(song_path, npd_path, sizeof(npd_path)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(npd_path, "rb");
    if (f == NULL) {
        ESP_LOGD(TAG, "no meta file: %s", npd_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 整个文件读入内存再解析（文件最大 256KB+，heap 分配一次） */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 12) {   /* 至少 4+4+4+4 = 16 字节，放宽到 12 由解析器拦截 */
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc((size_t)fsize);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    fclose(f);

    const uint8_t *p = buf;
    const uint8_t *end = buf + fsize;
    esp_err_t ret = ESP_ERR_INVALID_SIZE;

    /* 1. 标题 */
    if (end - p < 4) goto done;
    uint32_t tlen = meta_read_u32(p);
    p += 4;
    if (tlen > META_TEXT_MAX || (uint32_t)(end - p) < tlen) goto done;
    out->title = meta_copy_text(p, tlen);
    p += tlen;
    if (out->title == NULL && tlen > 0) goto done;

    /* 2. 副标题 */
    if (end - p < 4) goto done;
    uint32_t slen = meta_read_u32(p);
    p += 4;
    if (slen > META_TEXT_MAX || (uint32_t)(end - p) < slen) goto done;
    out->subtitle = meta_copy_text(p, slen);
    p += slen;
    if (out->subtitle == NULL && slen > 0) goto done;

    /* 3. 回放增益（uint32 线性值 × 65536） */
    if (end - p < 4) goto done;
    out->gain_q16 = meta_read_u32(p);
    p += 4;

    /* 4. JPEG 封面（长度 0 = 无封面） */
    if (end - p < 4) goto done;
    uint32_t jlen = meta_read_u32(p);
    p += 4;
    if (jlen > META_JPEG_MAX || (uint32_t)(end - p) < jlen) goto done;
    if (jlen > 0) {
        uint8_t *jd = heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (jd == NULL) {
            jd = malloc(jlen);
        }
        if (jd == NULL) goto done;
        memcpy(jd, p, jlen);
        out->jpeg_data = jd;
        out->jpeg_len = jlen;
    }

    ret = ESP_OK;
    ESP_LOGI(TAG, "meta loaded: '%s' | '%s' | gain %u.%04u | jpeg %u B",
             out->title ? out->title : "-", out->subtitle ? out->subtitle : "-",
             out->gain_q16 >> 16, (unsigned)(out->gain_q16 & 0xFFFF),
             (unsigned)out->jpeg_len);

done:
    free(buf);
    if (ret != ESP_OK) {
        song_meta_free(out);   /* 失败时清掉已分配字段 */
    }
    return ret;
}

void song_meta_free(song_meta_t *m)
{
    if (m == NULL) {
        return;
    }
    free(m->title);
    free(m->subtitle);
    free(m->jpeg_data);
    memset(m, 0, sizeof(*m));
}
