#include "core/player/shuffle_player.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "shuffle_player";

/* 缓存元素宽度切换阈值：歌曲总数不超过 65535 时缓存用 uint16_t（省一半 RAM），
   超过则退回 uint32_t（uint16_t 装不下）。 */
#define SHUFFLE_U16_MAX 65535u

/* ---------- 内部状态 ---------- */

static uint32_t  s_count         = 0;    /* 歌曲总数 N */
static void     *s_cache         = NULL; /* 当前块缓存（uint16_t* 或 uint32_t*） */
static bool      s_cache_u16     = false;/* true = 缓存为 uint16_t，false = uint32_t */
static uint64_t  s_cached_block  = 0;    /* 缓存对应的块索引 */
static bool      s_cached_valid  = false;/* 缓存是否已生成（首块未生成前为 false） */

/* ------------------------------------------------------------------ */
/* 确定性 PRNG（xorshift32）                                            */
/*                                                                     */
/* 仅要求本固件内部自洽：相同种子 → 相同序列，即可保证"保存位置可恢复"。  */
/* 不追求与 C#/Java 的 Random 兼容（设计文档注明跨语言一致性需自行保证）。 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state;
} shuffle_rng_t;

static void rng_seed(shuffle_rng_t *rng, uint32_t seed)
{
    /* xorshift 的 state 为 0 时输出恒为 0（洗牌退化为固定反序），必须规避 */
    rng->state = (seed != 0) ? seed : 0x9e3779b9u;
}

static uint32_t rng_next(shuffle_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

/* 均匀 [0, bound)：取模分布有微小偏差，对随机播放无感知影响 */
static uint32_t rng_range(shuffle_rng_t *rng, uint32_t bound)
{
    if (bound <= 1) {
        return 0;
    }
    return rng_next(rng) % bound;
}

/* ------------------------------------------------------------------ */
/* 块生成                                                              */
/* ------------------------------------------------------------------ */

/* 块索引 → 随机种子：位混合 + 黄金比例常数，相邻块种子差异大（文档 §5） */
static uint32_t shuffle_block_seed(uint64_t block_index)
{
    uint32_t x = (uint32_t)(block_index ^ (block_index >> 32));
    return x * 0x9e3779b9u;
}

/* 读写缓存元素（按当前元素宽度分派） */
static uint32_t shuffle_cache_get(uint32_t i)
{
    if (s_cache_u16) {
        return ((const uint16_t *)s_cache)[i];
    }
    return ((const uint32_t *)s_cache)[i];
}

static void shuffle_cache_set(uint32_t i, uint32_t v)
{
    if (s_cache_u16) {
        ((uint16_t *)s_cache)[i] = (uint16_t)v;
    } else {
        ((uint32_t *)s_cache)[i] = v;
    }
}

/* 用指定块索引生成该块的随机排列存入缓存（Fisher-Yates，确定性）。
   时间复杂度 O(N)，仅在跨块访问时触发。 */
static void shuffle_generate_block(uint64_t block_index)
{
    shuffle_rng_t rng;
    rng_seed(&rng, shuffle_block_seed(block_index));

    /* 初始化有序序列 [0, 1, ..., N-1] */
    for (uint32_t i = 0; i < s_count; i++) {
        shuffle_cache_set(i, i);
    }
    /* Fisher-Yates：从后往前，i 与 [0, i] 中随机位置交换 */
    for (uint32_t i = s_count - 1; i > 0; i--) {
        uint32_t j = rng_range(&rng, i + 1);
        uint32_t t = shuffle_cache_get(i);
        shuffle_cache_set(i, shuffle_cache_get(j));
        shuffle_cache_set(j, t);
    }

    s_cached_block = block_index;
    s_cached_valid = true;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t shuffle_player_init(uint32_t count)
{
    if (count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cache != NULL && s_count == count) {
        return ESP_OK;   /* 已按相同数量初始化（幂等） */
    }
    shuffle_player_deinit();

    /* 按歌曲数选择缓存元素宽度：≤65535 用 uint16_t 省内存 */
    s_cache_u16 = (count <= SHUFFLE_U16_MAX);
    size_t elem_size = s_cache_u16 ? sizeof(uint16_t) : sizeof(uint32_t);
    s_cache = malloc((size_t)count * elem_size);
    if (s_cache == NULL) {
        ESP_LOGE(TAG, "alloc cache failed (%u x %uB)",
                 (unsigned)count, (unsigned)elem_size);
        return ESP_ERR_NO_MEM;
    }
    s_count = count;
    s_cached_valid = false;
    ESP_LOGI(TAG, "shuffle ready: %u songs, cache %s (%uB)",
             (unsigned)count, s_cache_u16 ? "uint16" : "uint32",
             (unsigned)((size_t)count * elem_size));
    return ESP_OK;
}

void shuffle_player_deinit(void)
{
    free(s_cache);
    s_cache = NULL;
    s_count = 0;
    s_cached_valid = false;
}

int32_t shuffle_player_get_index_at(uint64_t position)
{
    if (s_cache == NULL || s_count == 0) {
        return -1;
    }
    uint64_t block = position / s_count;
    uint32_t offset = (uint32_t)(position % s_count);
    if (!s_cached_valid || s_cached_block != block) {
        shuffle_generate_block(block);
    }
    return (int32_t)shuffle_cache_get(offset);
}

int32_t shuffle_player_find_in_block(uint64_t block_index, uint32_t global)
{
    if (s_cache == NULL || s_count == 0) {
        return -1;
    }
    if (global >= s_count) {
        return -1;
    }
    if (!s_cached_valid || s_cached_block != block_index) {
        shuffle_generate_block(block_index);
    }
    for (uint32_t i = 0; i < s_count; i++) {
        if (shuffle_cache_get(i) == global) {
            return (int32_t)i;
        }
    }
    return -1;   /* 理论不可达：块内包含全部歌曲 */
}

uint64_t shuffle_player_random_start(void)
{
    /* 硬件真随机数拼 64 位：每次调用返回 32 位，两次拼出完整位置 */
    uint64_t hi = esp_random();
    uint64_t lo = esp_random();
    return (hi << 32) | lo;
}
