#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libogg/Tremor 的堆分配入口：由 os_types.h 的 _ogg_malloc 等宏指向这里。
   OGG 播放的所有解码内存（含 ogg_sync / codebook / pcm 缓冲）都走
   1MB PSRAM 专用堆，避免与 SDMMC 的 DMA 池争抢内部 RAM。 */

/** @brief 初始化 1MB PSRAM 专用堆（幂等；未调用时分配函数自动退回逐块 PSRAM） */
void ogg_psram_alloc_init(void);

void *ogg_psram_malloc(size_t size);
void *ogg_psram_calloc(size_t nmemb, size_t size);
void *ogg_psram_realloc(void *ptr, size_t size);
void ogg_psram_free(void *ptr);

#ifdef __cplusplus
}
#endif
