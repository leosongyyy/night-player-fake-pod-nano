#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file shuffle_player.h
 * @brief 无限确定性随机播放列表（分块 + 确定性洗牌）
 *
 * 实现设计文档《无限确定性随机播放列表》（参考"随机播放.md"）：
 *   - 把无限播放序列看作无数个"块"（每个块是**当前随机列表**歌曲的一个
 *     随机排列）首尾相连：Block_0, Block_1, ...
 *   - 逻辑位置 P 映射到 块索引 = P / N、块内偏移 = P % N（N = 当前随机
 *     列表的歌曲数，由调用方按"当前播放目录"传入）
 *   - 每个块只依赖块索引：以 块索引哈希 作为伪随机种子，对 [0..N-1]
 *     做 Fisher-Yates 洗牌，得到该块的排列。给定同一位置，结果永远一致
 *     （确定性），因此只需保存一个 uint64 位置即可"断点续播 / 回到上一曲"。
 *   - 内存占用 O(N)：只缓存当前所在块；跨块时按需重洗。
 *
 * 设计取舍（嵌入式）：
 *   - PRNG 用 xorshift32（确定性、极快、无外部依赖）；块间种子用
 *     位混合 + 黄金比例常数，相邻块种子差异大（文档 §3.3/§5 同款）。
 *   - 缓存元素按列表歌曲数缩水：N ≤ 65535 时用 uint16_t（2B/元素），
 *     否则退回 uint32_t，兼顾内存与容量上限。
 *   - 洗牌取模分布有微小偏差，对随机播放无感知影响，换取确定性。
 */

/**
 * @brief 初始化随机列表（分配块缓存并记录歌曲总数）
 *
 * 可重复调用：歌曲总数未变时直接返回（幂等）；总数变化时重建缓存。
 *
 * @param count 随机列表的歌曲数（当前播放目录的歌曲数，须 > 0；
 *              缓存仅保存 [0..count-1] 的排列，与具体目录无关，
 *              目录切换后排列可安全复用）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG count=0；ESP_ERR_NO_MEM 缓存分配失败
 */
esp_err_t shuffle_player_init(uint32_t count);

/** @brief 释放块缓存（可重复调用） */
void shuffle_player_deinit(void);

/**
 * @brief 逻辑位置 → 原列表歌曲下标（GetIndexAt）
 *
 * 若目标位置不在当前缓存块内，自动重洗生成新块（O(N)，仅在跨块时发生）。
 *
 * @param position 逻辑播放位置（>= 0，uint64 可视为"无限"）
 * @return 原列表歌曲下标 [0, count)；-1 未初始化 / 参数非法
 */
int32_t shuffle_player_get_index_at(uint64_t position);

/**
 * @brief 在指定块内查找目标歌曲的偏移（用于"重新随机并定位到指定歌曲"）
 *
 * 会先按该块索引重洗（若与当前缓存块不同），再线性查找。
 *
 * @param block_index 块索引（作为该块的随机种子来源）
 * @param global      目标歌曲在原列表中的下标
 * @return 该歌曲在块内的偏移 [0, count)；-1 未初始化 / 歌曲越界
 */
int32_t shuffle_player_find_in_block(uint64_t block_index, uint32_t global);

/**
 * @brief 生成一个随机的起始逻辑位置（建议在"重新开始随机播放"时调用）
 *
 * 用硬件真随机数（esp_random）拼 64 位，保证每次开启随机序列都不同。
 *
 * @return 非负随机位置
 */
uint64_t shuffle_player_random_start(void);

#ifdef __cplusplus
}
#endif
