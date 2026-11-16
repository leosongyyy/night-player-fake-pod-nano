#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/storage/playlist_format.h"   /* PLS_STR_MAX_OFFSET：数据本体上限 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表存储（包装 pldata 后端，提供"整体重写 + 头部 commit"语义）
 *
 * 镜像布局（自 2026-11-16 起存于 SD 卡文件 `/sdcard/.ecov/pldata.bin`，
 * 与旧的 4MB `pldata` flash 分区镜像逐字节相同）：
 *
 *     offset 0x0000 │ 头部扇区（1 个 4KB 扇区）  │ ← commit 标志
 *     offset 0x1000 │ 数据区（PLST 数据本体）    │ ← 播放列表数据
 *
 * 设计要点：
 *  - **is_valid** 通过头部 magic / 版本 / 长度，外加**整块 CRC32**（SD 会坏，
 *    值得校验一次；620KB ≈ 2ms）判断数据是否有效。CRC 字段为 0 视为"老固件
 *    没填"，跳过校验。
 *  - **invalidate** 删除卡上缓存文件，使数据立刻失效。
 *  - **save_playlist** 一次性整体重写：先擦头部（内存缓冲立即失效）→ 再擦/写
 *    数据区 → 写头部（commit）→ 最后 `pldata_commit()` 落盘（tmp + rename 原子
 *    替换）。中途断电时卡上要么是完整旧文件、要么没有文件，绝不会是半截数据。
 *  - **map_to_memory** 返回整份镜像的基址（PSRAM，只读访问）。
 *
 * 注意：save_playlist 是一次性整写；数据区起点 offset 0x1000（4KB 对齐）。
 */

/** 数据本体最大长度 = 构建缓冲上限（4MB，见 playlist_format.h PLS_STR_MAX_OFFSET） */
#define PLAYLIST_STORE_MAX_DATA ((size_t)PLS_STR_MAX_OFFSET)

/** 数据区起始偏移（紧跟头部扇区，扇区对齐） */
#define PLAYLIST_STORE_DATA_OFFSET 0x1000U

/** 数据区最大长度按扇区向上取整后的擦除大小（可配合 save 使用） */
#define PLAYLIST_STORE_DATA_ERASE_SZ(len) \
    (((len) + 0x0FFFU) & ~0x0FFFU)

/**
 * @brief 播放列表缓存元信息（与数据同一次 commit 写入头部）
 *
 * 开机时用它做"是否需要重建"的快速判断：
 *   - card_id 与当前 SD 卡不符 → 换卡，必须重建；
 *   - signature / file_count 与曲库指纹探测结果不符 → 增删/改名/换文件，
 *     必须重建。
 * 未变化时跳过完整扫描直接启动。
 */
typedef struct {
    uint64_t card_id;     /* SD 卡 CID 派生 ID（见 sd_card_get_id） */
    uint64_t signature;   /* 曲库指纹（音频路径 + .pl 的路径/大小/mtime 之和） */
    uint32_t file_count;  /* 物理音频文件数（不含 .pl 内引用） */
    uint32_t reserved;    /* 对齐/将来扩展，写 0 */
} playlist_store_meta_t;

/**
 * @brief 判断缓存是否有效（头部 + CRC32 校验；快速，不解析数据）
 *
 * @return true 有效；false 无效（卡上无文件 / 已 invalidate / 被中断 / CRC 坏）
 */
bool playlist_store_is_valid(void);

/**
 * @brief 使缓存立刻失效（删除卡上缓存文件）
 *
 * 调用后 is_valid() 返回 false，直到下次 save_playlist() 成功。
 *
 * @return ESP_OK 成功（文件本来不存在也算成功）
 */
esp_err_t playlist_store_invalidate(void);

/**
 * @brief 一次性整体保存播放列表数据
 *
 * 内部顺序保证"中断安全"：
 *   1. 先擦头部（内存缓冲立即失效）→ 2. 按 len 对齐擦数据区 → 3. 写数据 →
 *   4. 写头部（commit）→ 5. `pldata_commit()` 落盘（tmp → fsync → rename）。
 * 中途断电中断，卡上要么是旧的完整文件，要么没有文件。
 *
 * @param data 内存中的源数据
 * @param len  数据长度（须 ≤ PLAYLIST_STORE_MAX_DATA）
 * @param meta 缓存元信息（卡 ID / 曲库指纹；可为 NULL，写 0 表示未知）
 * @return ESP_OK 成功
 */
esp_err_t playlist_store_save_playlist(const void *data, size_t len,
                                       const playlist_store_meta_t *meta);

/**
 * @brief 读取播放列表缓存元信息
 *
 * 只在 is_valid() 为 true 时有意义；头部版本不匹配/损坏返回 false。
 *
 * @param[out] out 输出元信息
 * @return true 成功
 */
bool playlist_store_get_meta(playlist_store_meta_t *out);

/**
 * @brief 拿到整份镜像的基址（PSRAM，只读）
 *
 * 返回的基址指向镜像开头（offset 0），数据本体位于
 * PLAYLIST_STORE_DATA_OFFSET 处。请先用 is_valid() / get_data_len()
 * 确认有效性与长度，再读取。
 *
 * @param[out] out_ptr 返回镜像基址（指向 offset 0）
 * @return ESP_OK 成功
 */
esp_err_t playlist_store_map_to_memory(const void **out_ptr);

/**
 * @brief 读取有效数据的长度（来自头部）
 *
 * @return 有效数据的字节数；无效或尚未 init 时为 0
 */
size_t playlist_store_get_data_len(void);

#ifdef __cplusplus
}
#endif
