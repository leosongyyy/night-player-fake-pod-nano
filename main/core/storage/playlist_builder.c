#include "core/storage/playlist_builder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "core/storage/playlist_format.h"
#include "core/storage/playlist_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "playlist_builder";

/* 构建缓冲总大小（字符串池起点到池上限，约 3.7MB，放 PSRAM） */
#define PLS_BUILD_BUF_SIZE PLS_STR_MAX_OFFSET

/* 单目录内文件/子目录数量上限 */
#define PLS_MAX_ENTRIES 4096u
/* 遍历最大深度（用显式栈，非递归） */
#define PLS_MAX_DEPTH 512u
/* 相对路径缓冲长度 */
#define PLS_PATH_MAX 512u
/* 池可用最大长度（字符串池起点到上限） */
#define PLS_STR_CAPACITY (PLS_STR_MAX_OFFSET - PLS_STR_OFFSET)

/* 无效的字符串池索引 */
#define PLS_STR_INVALID UINT32_MAX

/* 显式栈节点：一个待处理目录 */
typedef struct {
    char     path[PLS_PATH_MAX];  /* 相对根目录的完整路径（不含 /sdcard，根目录为空串） */
    uint32_t path_idx;            /* 该目录路径在字符串池的 index（作歌曲 path_str） */
    uint32_t name_idx;            /* 该目录短名在字符串池的 index（作目录条目 name_str） */
} build_node_t;

/* 构建上下文（所有游标/缓冲都存这里，便于在 PSRAM 里） */
typedef struct {
    uint8_t  *buf;        /* 整个 PLST 数据的构建缓冲（PSRAM） */
    uint32_t  dir_count;  /* 目录条目游标 */
    uint32_t  song_count; /* 歌曲条目游标 */
    uint32_t  str_len;    /* 字符串池已用长度 */
    bool      truncated;
} build_ctx_t;

/* 构建栈（显式，PSRAM 分配，替代递归） */
static build_node_t *s_stack = NULL;
static int s_sp = 0;

/* ---------------- 字符串池 ---------------- */

/* 向池尾部追加一个 null-terminated 字符串，返回相对池起点的 index */
static uint32_t str_add(build_ctx_t *ctx, const char *s)
{
    size_t n = strlen(s) + 1;
    if (ctx->str_len + n > PLS_STR_CAPACITY) {
        ctx->truncated = true;
        return PLS_STR_INVALID;
    }
    uint32_t idx = ctx->str_len;
    memcpy(ctx->buf + PLS_STR_OFFSET + idx, s, n);
    ctx->str_len += (uint32_t)n;
    return idx;
}

/* ---------------- 排序 key（名称前 4 字节当 uint，不足补 0） ---------------- */
/* 排序时把 ASCII 大写折叠为小写，使排序大小写不敏感（仅影响顺序，不改存储原文） */
static uint32_t name_key(const char *s)
{
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) {
        k <<= 8;
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + ('a' - 'A'));
        }
        k |= c;   /* 长度不足 4 时，后续 '\0' 补为 0 */
    }
    return k;
}

static int cmp_name(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    uint32_t ka = name_key(sa), kb = name_key(sb);
    if (ka != kb) {
        return ka < kb ? -1 : 1;
    }
    return strcasecmp(sa, sb);   /* 大小写不敏感次级比较 */
}

/* ---------------- 路径辅助 ---------------- */

/* 相对路径 → SD 完整路径（拼 /sdcard 前缀） */
static void make_full_path(const char *rel, char *out, size_t outsz)
{
    if (rel[0]) {
        snprintf(out, outsz, "/sdcard/%s", rel);
    } else {
        snprintf(out, outsz, "/sdcard");
    }
}

/* ---------------- 音频扩展名判断（当前播放引擎 Helix 仅支持 MP3） ---------------- */

static bool is_audio_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        return false;
    }
    const char *ext = dot + 1;
    /* 音乐库目前只有 .mp3，播放引擎也只支持 MP3，故仅收录 mp3 */
    return (strcasecmp(ext, "mp3") == 0);
}

/* ---------------- 播放列表（.pl）文件 ---------------- */

/* 判断是否为 .pl 播放列表文件 */
static bool is_pl_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        return false;
    }
    return (strcasecmp(dot + 1, "pl") == 0);
}

/* 去掉行尾 \r\n 与首尾空白；若整行空白则得到空串 */
static void strip_line(char *line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != line) {
        memmove(line, p, strlen(p) + 1);
    }
}

/* 解析单个 .pl 播放列表：每行是一个相对 SD 根目录的路径（也可能是空行）。
   按"文件名 + 路径前缀"拆分存储（列表显示文件名方便，且同路径的歌连续出现）。
   该 .pl 生成一个播放列表目录条目，其歌曲为一个连续歌曲区间。 */
static void scan_pl_file(build_ctx_t *ctx, const char *dir_full, const char *pl_name)
{
    char path[PLS_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", dir_full, pl_name);
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "open playlist '%s' failed", path);
        return;
    }

    uint32_t pl_name_idx = str_add(ctx, pl_name);   /* 播放列表名（含 .pl） */
    uint32_t start = ctx->song_count;

    playlist_song_entry_t *songs = (playlist_song_entry_t *)(ctx->buf + PLS_SONG_OFFSET);
    playlist_dir_entry_t *dirs = (playlist_dir_entry_t *)(ctx->buf + PLS_DIR_OFFSET);

    char line[PLS_PATH_MAX + 64];
    char prev_path[PLS_PATH_MAX] = "";
    uint32_t prev_path_idx = UINT32_MAX;
    uint32_t pl_songs = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        strip_line(line);
        if (line[0] == '\0') {
            continue;   /* 跳过空行/空白行 */
        }
        /* 拆分为 文件名 + 路径前缀 */
        char *slash = strrchr(line, '/');
        const char *fname = slash ? slash + 1 : line;
        char pathpart[PLS_PATH_MAX];
        if (slash) {
            size_t plen = (size_t)(slash - line);
            memcpy(pathpart, line, plen);
            pathpart[plen] = '\0';
        } else {
            pathpart[0] = '\0';
        }

        uint32_t path_idx;
        if (pathpart[0] == '\0') {
            path_idx = 0;   /* 无路径 → 空串索引 */
        } else if (strcmp(pathpart, prev_path) == 0) {
            path_idx = prev_path_idx;   /* 同路径连续出现，复用字符串池条目 */
        } else {
            path_idx = str_add(ctx, pathpart);
            prev_path_idx = path_idx;
            memcpy(prev_path, pathpart, strlen(pathpart) + 1);
        }

        if (ctx->song_count >= PLS_SONG_MAX_COUNT) {
            ctx->truncated = true;
            break;
        }
        uint32_t name_idx = str_add(ctx, fname);
        if (name_idx == PLS_STR_INVALID || path_idx == PLS_STR_INVALID) {
            ctx->truncated = true;
            break;
        }
        songs[ctx->song_count].name_str = name_idx;
        songs[ctx->song_count].path_str = path_idx;
        ctx->song_count++;
        pl_songs++;
    }
    fclose(f);

    if (pl_songs > 0 && ctx->dir_count < PLS_DIR_MAX_COUNT) {
        dirs[ctx->dir_count].name_str = pl_name_idx;
        dirs[ctx->dir_count].song_start = start;
        dirs[ctx->dir_count].song_count = pl_songs;
        ctx->dir_count++;
    } else if (pl_songs > 0) {
        ctx->truncated = true;
    }
    ESP_LOGI(TAG, "playlist '%s': %u songs", pl_name, (unsigned)pl_songs);
}

/* ---------------- 单目录处理：列出文件与子目录 ---------------- */

/* 用 stat 判断条目类型，收集音频文件 / .pl 播放列表 / 子目录，并各自排序。
   files/pls/subs 为预先分配（cap）的指针数组，返回各自数量。 */
static void collect_dir(const char *full_path,
                        char **files, int *nf,
                        char **pls, int *npl,
                        char **subs, int *ns, int cap)
{
    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGW(TAG, "opendir '%s' failed", full_path);
        return;
    }
    char entry_path[PLS_PATH_MAX + 64];
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && *nf < cap && *ns < cap && *npl < cap) {
        const char *name = e->d_name;
        if (name[0] == '.') {
            continue;   /* 跳过 . .. 及隐藏文件 */
        }
        /* 构造完整路径后用 stat 判断类型（FAT 的 d_type 不可靠） */
        if (full_path[strlen(full_path) - 1] == '/') {
            snprintf(entry_path, sizeof(entry_path), "%s%s", full_path, name);
        } else {
            snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, name);
        }
        struct stat st;
        if (stat(entry_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            subs[(*ns)++] = strdup(name);
        } else if (S_ISREG(st.st_mode)) {
            if (is_audio_file(name)) {
                files[(*nf)++] = strdup(name);
            } else if (is_pl_file(name)) {
                pls[(*npl)++] = strdup(name);
            }
        }
    }
    closedir(dir);

    /* 各自按名称前 4 字节排序 */
    qsort(files, (size_t)*nf, sizeof(char *), cmp_name);
    qsort(pls, (size_t)*npl, sizeof(char *), cmp_name);
    qsort(subs, (size_t)*ns, sizeof(char *), cmp_name);
}

/* ---------------- 主构建入口 ---------------- */

static esp_err_t playlist_build_core(playlist_build_stats_t *stats, bool save,
                                     const void **out_buf, uint32_t *out_size,
                                     playlist_build_progress_cb progress_cb,
                                     void *progress_ctx)
{
    build_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 构建缓冲（PSRAM，约 3.7MB），全部数据在此组装后一次性写入分区 */
    ctx.buf = heap_caps_malloc(PLS_BUILD_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.buf) {
        ESP_LOGE(TAG, "alloc build buffer failed");
        return ESP_ERR_NO_MEM;
    }

    /* 显式栈（PSRAM），替代递归遍历 */
    s_stack = heap_caps_malloc(sizeof(build_node_t) * PLS_MAX_DEPTH,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stack) {
        free(ctx.buf);
        ESP_LOGE(TAG, "alloc build stack failed");
        return ESP_ERR_NO_MEM;
    }
    s_sp = 0;

    /* 字符串池首字节为 0x00（空字符串索引 0），str_len 从 1 开始 */
    ctx.str_len = 1;
    ctx.buf[PLS_STR_OFFSET] = 0;

    /* 先初始化头部（生成时先填充分区布局信息，扫描完成后回填统计） */
    playlist_header_t *hdr = (playlist_header_t *)ctx.buf;
    memcpy(hdr->magic, PLS_MAGIC, 4);
    hdr->dir_offset = PLS_DIR_OFFSET;
    hdr->dir_count = 0;
    hdr->song_offset = PLS_SONG_OFFSET;
    hdr->song_capacity = PLS_SONG_MAX_COUNT;
    hdr->str_offset = PLS_STR_OFFSET;
    hdr->str_len = 0;

    /* 根目录入栈（path 空、path_idx=0=空串、name_idx=0=空串） */
    build_node_t *root = &s_stack[s_sp++];
    root->path[0] = '\0';
    root->path_idx = 0;
    root->name_idx = 0;

    /* 每目录收集用的指针数组（heap 分配，扫描完释放） */
    int cap = PLS_MAX_ENTRIES;
    char **files = calloc((size_t)cap, sizeof(char *));
    char **pls = calloc((size_t)cap, sizeof(char *));
    char **subs = calloc((size_t)cap, sizeof(char *));
    if (!files || !pls || !subs) {
        free(files); free(pls); free(subs); free(s_stack); free(ctx.buf);
        ESP_LOGE(TAG, "alloc entry arrays failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "start scanning /sdcard ...");
    uint32_t scanned_dirs = 0;

    while (s_sp > 0) {
        build_node_t *node = &s_stack[--s_sp];

        char full_path[PLS_PATH_MAX + 2];
        make_full_path(node->path, full_path, sizeof(full_path));

        int nf = 0, ns = 0, npl = 0;
        collect_dir(full_path, files, &nf, pls, &npl, subs, &ns, cap);
        scanned_dirs++;
        ESP_LOGI(TAG, "scan dir='%s' files=%d pls=%d subdirs=%d", node->path, nf, npl, ns);

        /* 1. 先处理本目录的 .pl 播放列表（每个 .pl → 一个播放列表目录条目） */
        for (int i = 0; i < npl; i++) {
            scan_pl_file(&ctx, full_path, pls[i]);
        }

        /* 2. 再写本目录的音频文件（作为歌曲），记录该目录的歌曲区间 */
        if (nf > 0) {
            if (ctx.song_count + (uint32_t)nf > PLS_SONG_MAX_COUNT ||
                ctx.dir_count >= PLS_DIR_MAX_COUNT) {
                ctx.truncated = true;
            } else {
                uint32_t dir_start = ctx.song_count;
                playlist_song_entry_t *songs =
                    (playlist_song_entry_t *)(ctx.buf + PLS_SONG_OFFSET);
                for (int i = 0; i < nf; i++) {
                    uint32_t name_idx = str_add(&ctx, files[i]);
                    if (name_idx == PLS_STR_INVALID) {
                        break;
                    }
                    songs[ctx.song_count].name_str = name_idx;
                    songs[ctx.song_count].path_str = node->path_idx; /* 同目录共享路径 */
                    ctx.song_count++;
                }
                /* 写目录条目（指向本目录歌曲区间） */
                playlist_dir_entry_t *dirs =
                    (playlist_dir_entry_t *)(ctx.buf + PLS_DIR_OFFSET);
                dirs[ctx.dir_count].name_str = node->name_idx;
                dirs[ctx.dir_count].song_start = dir_start;
                dirs[ctx.dir_count].song_count = ctx.song_count - dir_start;
                ctx.dir_count++;
            }
        }

        /* 后递归子目录（逆序入栈，保证出栈时按名称顺序） */
        /* ⚠ push 的第一个 child 会写入 &s_stack[--s_sp] 弹出的位置（node 所在槽位），
           可能覆盖 node->path。故先把父路径拷贝到局部变量，用副本拼接子路径。 */
        char parent_path[PLS_PATH_MAX];
        memcpy(parent_path, node->path, strlen(node->path) + 1);
        for (int i = ns - 1; i >= 0; i--) {
            if (s_sp >= PLS_MAX_DEPTH) {
                ctx.truncated = true;
                break;
            }
            build_node_t *child = &s_stack[s_sp++];
            char child_path[PLS_PATH_MAX];
            if (parent_path[0]) {
                snprintf(child_path, sizeof(child_path), "%s/%s", parent_path, subs[i]);
            } else {
                snprintf(child_path, sizeof(child_path), "%s", subs[i]);
            }
            memcpy(child->path, child_path, strlen(child_path) + 1);
            child->name_idx = str_add(&ctx, subs[i]);
            child->path_idx = str_add(&ctx, child->path);
        }

        /* 释放本目录收集的字符串 */
        for (int i = 0; i < nf; i++) {
            free(files[i]);
        }
        for (int i = 0; i < npl; i++) {
            free(pls[i]);
        }
        for (int i = 0; i < ns; i++) {
            free(subs[i]);
        }

        /* 每处理完一个目录，通知一次扫描进度（供 UI 实时显示已扫描音乐数） */
        if (progress_cb != NULL) {
            progress_cb(PLAYLIST_BUILD_PHASE_SCAN, ctx.song_count, progress_ctx);
        }
    }

    /* 回填头部统计 */
    hdr->dir_count = ctx.dir_count;
    hdr->str_len = ctx.str_len;

    uint32_t total_size = PLS_STR_OFFSET + ctx.str_len;

    ESP_LOGI(TAG, "scan done: dirs=%u songs=%u str_len=%u total=%u%s",
             ctx.dir_count, ctx.song_count, ctx.str_len, total_size,
             ctx.truncated ? " [TRUNCATED]" : "");

    /* 统一回填统计 */
    if (stats) {
        stats->dir_count = ctx.dir_count;
        stats->song_count = ctx.song_count;
        stats->str_len = ctx.str_len;
        stats->total_size = total_size;
        stats->truncated = ctx.truncated;
    }

    if (save) {
        /* 写回模式：整体写入分区（playlist_store：先擦后写，中断安全），随后释放。
           进入写入阶段前通知一次，供 UI 提示"正在写入扫描数据" */
        if (progress_cb != NULL) {
            progress_cb(PLAYLIST_BUILD_PHASE_WRITE, ctx.song_count, progress_ctx);
        }
        ESP_LOGI(TAG, "saving playlist to partition (len=%u) ...", total_size);
        esp_err_t ret = playlist_store_save_playlist(ctx.buf, total_size);
        free(files); free(pls); free(subs); free(s_stack); free(ctx.buf);
        s_stack = NULL;
        return ret;
    }

    /* 内存模式：不写回，返回 buffer 供调用方（本任务/测试）直接解析读取；
       buffer 由调用方负责 free()。 */
    *out_buf = ctx.buf;
    *out_size = total_size;
    free(files); free(pls); free(subs); free(s_stack);
    s_stack = NULL;
    return ESP_OK;
}

/* 扫描构建并写回 pldata 分区 */
esp_err_t playlist_build_from_sd(playlist_build_stats_t *stats,
                                 playlist_build_progress_cb progress_cb,
                                 void *progress_ctx)
{
    return playlist_build_core(stats, true, NULL, NULL, progress_cb, progress_ctx);
}

/* 仅扫描构建到内存（不写回），返回 buffer 供内存解析测试 */
esp_err_t playlist_build_in_memory(playlist_build_stats_t *stats,
                                   const void **out_buf, uint32_t *out_size)
{
    return playlist_build_core(stats, false, out_buf, out_size, NULL, NULL);
}
