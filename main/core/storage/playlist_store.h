#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表存储（包装 pldata 分区，提供"整体重写 + 头部 commit"语义）
 *
 * 分区布局（4MB，见 partitions.csv 的 `pldata`）：
 *
 *     offset 0x0000 │ 头部扇区（1 个 4KB 扇区）  │ ← commit 标志
 *     offset 0x1000 │ 数据区（最多 4MB - 4KB）  │ ← 播放列表数据本体
 *
 * 设计要点：
 *  - **is_valid** 通过头部 magic / 长度合法性判断分区数据是否有效。
 *  - **invalidate** 只擦头部扇区，使分区立刻变为"无效"（数据暂不可用）。
 *  - **save_playlist** 一次性整体重写：先擦头部（立即失效）→ 再擦/写数据区 →
 *    最后写头部（commit）。中途任何一步断电中断，头部都处于擦除态，
 *    分区始终表现为"无效"，不会读到半截数据。
 *  - **map_to_memory** 把整个分区内存映射（只读、省 RAM），数据留在 Flash。
 *
 * 注意：save_playlist 是一次性整写；对已写入区域重复保存会先整体擦除，安全。
 * 数据区起点 offset 0x1000（对齐扇区），头部单独占一个扇区。
 */

/** 分区中可容纳的最大数据长度（4MB - 1 个头部扇区） */
#define PLAYLIST_STORE_MAX_DATA (PLDATA_PART_SIZE - 0x1000U)

/** 数据区起始偏移（紧跟头部扇区，扇区对齐） */
#define PLAYLIST_STORE_DATA_OFFSET 0x1000U

/** 数据区最大长度按扇区向上取整后的擦除大小（可配合 save 使用） */
#define PLAYLIST_STORE_DATA_ERASE_SZ(len) \
    (((len) + 0x0FFFU) & ~0x0FFFU)

/**
 * @brief 判断分区是否有效（读取头部，检查 magic 与长度合法性；快速，不读数据）
 *
 * @return true 有效；false 无效（从未写入 / 已 invalidate / 被中断）
 */
bool playlist_store_is_valid(void);

/**
 * @brief 使分区暂时无效（只擦头部扇区，数据区保留）
 *
 * 调用后 is_valid() 返回 false，直到下次 save_playlist() 成功。
 *
 * @return ESP_OK 成功
 */
esp_err_t playlist_store_invalidate(void);

/**
 * @brief 一次性整体保存播放列表数据
 *
 * 内部顺序保证"中断安全"：
 *   1. 先擦头部（数据立即失效）→ 2. 按 len 对齐扇区擦数据区 → 3. 写数据 →
 *   4. 最后写头部（commit，此时才生效）。
 * 中途断电中断，头部保持擦除态，分区始终无效。
 *
 * @param data 内存中的源数据
 * @param len  数据长度（须 ≤ PLAYLIST_STORE_MAX_DATA）
 * @return ESP_OK 成功
 */
esp_err_t playlist_store_save_playlist(const void *data, size_t len);

/**
 * @brief 把整个分区内存映射到地址空间，返回只读基址（省 RAM）
 *
 * 返回的基址指向分区开头（offset 0），数据本体位于
 * PLAYLIST_STORE_DATA_OFFSET 处。请先用 is_valid() / get_data_len()
 * 确认有效性与长度，再读取。
 *
 * @param[out] out_ptr 返回映射基址（指向分区开头）
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
