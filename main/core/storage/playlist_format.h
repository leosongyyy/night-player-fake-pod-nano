#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表数据格式（NightPlayer 核心设计）
 *
 * 数据是一份**固定布局**的二进制块，作为整体存入 pldata 后端的
 * `playlist_store` 数据区（SD 卡缓存文件 `/sdcard/.ecov/pldata.bin`，
 * 开机整块读进 PSRAM，从 PLAYLIST_STORE_DATA_OFFSET 起）。
 * 读取时直接用指针按本格式解析（不复制数据）。
 *
 * 分区地址空间（约 4MB，偏移相对"播放列表数据起点"）：
 *
 *    0x00       头部（0x30 字节，PLST magic）
 *    0x30       目录区（到 0x10000，约 65488 字节 ≈ 5457 个目录条目）
 *    0x10000    歌曲索引区（到 0x40000，约 192KB ≈ 24576 首）
 *    0x40000    字符串常量池（到 0x400000，约 3.75MB，null-terminated 串依次排列）
 *
 * ⚠ 字符串池上限（PLS_STR_MAX_OFFSET）即开机扫描构建播放列表时分配的
 *    PSRAM 缓冲大小。历史：S3 时代完整版固件（完整字体 + RGB565 封面经
 *    SPIRAM_RODATA 复制进 PSRAM）内存紧张，曾从 0x3f0000（~3.7MB）降到
 *    0x2cc000（~2.8MB）。S31（16MB Octal PSRAM / 200MHz）起不再有这条约束，
 *    且播放列表已搬到 SD 卡（见 ideas_and_exps/PLDATA_ON_SDCARD.md），
 *    2026-11-16 起恢复为 4MB，池容量 3.75MB。
 *
 * 目录条目（12 字节）：{ 目录名池偏移(4) + 歌曲起始下标(4) + 歌曲数(4) }
 * 歌曲条目（8 字节）： { 文件名池偏移(4) + 父目录路径池偏移(4) }
 *
 * 所有"字符串池位置"字段均指**相对字符串池起点(0x40000)**的偏移，
 * 池第一个字节为 0x00，即"空字符串"索引 0。
 * 歌曲路径为相对根目录的路径（不含 /sdcard 前缀），如 "musics/games/xxx"，
 * 播放时拼接 "/sdcard/" + path + "/" + name。
 */

/* ---------- 头部魔数与偏移 ---------- */
#define PLS_MAGIC            "PLST"       /* 0x00~0x04 */
#define PLS_HEADER_SIZE      0x30u        /* 头部固定 48 字节 */

#define PLS_DIR_OFFSET       0x30u        /* 目录区起点（固定） */
#define PLS_DIR_AREA_SIZE    (0x10000u - PLS_DIR_OFFSET)   /* 65488 字节 */
#define PLS_SONG_OFFSET      0x10000u     /* 歌曲索引区起点 */
#define PLS_SONG_AREA_SIZE   (0x40000u - PLS_SONG_OFFSET)  /* 192KB */
#define PLS_STR_OFFSET       0x40000u     /* 字符串池起点 */
/* 字符串池写入上限（绝对偏移）：= 构建缓冲大小。
   ⚠ 这个值决定"曲库字符串总量上限"，不是分区大小。S31 的 16MB PSRAM 完全
   扛得住 4MB 缓冲（稳态空闲 16MB），2026-11-16 由 0x2cc000（2.8MB）恢复为
   4MB（池容量 3.75MB ≈ 十几万首曲目路径，远超 SD 卡实际容量能装的曲目数）。 */
#define PLS_STR_MAX_OFFSET   0x400000u

/* 目录/歌曲最大条数（按条目大小估算） */
#define PLS_DIR_MAX_COUNT    (PLS_DIR_AREA_SIZE / sizeof(playlist_dir_entry_t))
#define PLS_SONG_MAX_COUNT   (PLS_SONG_AREA_SIZE / sizeof(playlist_song_entry_t))

/* ---------- 头部结构（packed，严格对应 0x30 字节布局） ---------- */
typedef struct __attribute__((packed)) {
    char     magic[4];        /* 0x00 "PLST" */
    uint32_t dir_offset;      /* 0x04 目录偏移（固定 0x30） */
    uint16_t dir_count;       /* 0x08 实际目录数量 */
    uint32_t song_offset;     /* 0x0A 歌曲索引区偏移（固定 0x10000） */
    uint32_t song_capacity;   /* 0x0E 该段能装下歌曲数量（容量） */
    uint32_t str_offset;      /* 0x12 字符串池偏移（固定 0x40000） */
    uint32_t str_len;         /* 0x16 字符串池已用长度 */
    uint8_t  reserved[0x30u - 0x1Au]; /* 0x1A~0x2F 保留 */
} playlist_header_t;

/* 目录条目（12 字节） */
typedef struct __attribute__((packed)) {
    uint32_t name_str;    /* 目录名称在字符串池的位置（相对池起点） */
    uint32_t song_start;  /* 目录第一首歌在歌曲数组的下标 */
    uint32_t song_count;  /* 目录内歌曲数量 */
} playlist_dir_entry_t;

/* 歌曲条目（8 字节） */
typedef struct __attribute__((packed)) {
    uint32_t name_str;    /* 歌曲文件名（含扩展名）在字符串池的位置 */
    uint32_t path_str;    /* 父目录路径（不含 /sdcard）在字符串池的位置 */
} playlist_song_entry_t;

#ifdef __cplusplus
}
#endif
