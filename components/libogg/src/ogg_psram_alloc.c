#include "ogg/ogg_alloc.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "multi_heap.h"

#define OGG_ARENA_SIZE (1024 * 1024)
#define OGG_ARENA_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "ogg_alloc";

static void *s_arena;
static multi_heap_handle_t s_heap;
static SemaphoreHandle_t s_lock;
static bool s_init_done;
static bool s_warned_exhausted;

static inline bool in_arena(const void *p)
{
    return s_arena
        && (const char *)p >= (const char *)s_arena
        && (const char *)p < (const char *)s_arena + OGG_ARENA_SIZE;
}

static inline void arena_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void arena_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void ogg_psram_alloc_init(void)
{
    if (s_init_done) {
        return;
    }
    /* 只在播放任务里、ov_open 之前调用一次，无需额外并发保护 */
    s_init_done = true;

    s_arena = heap_caps_malloc(OGG_ARENA_SIZE, OGG_ARENA_CAPS);
    if (!s_arena) {
        ESP_LOGW(TAG, "1MB PSRAM arena alloc failed, using per-block PSRAM");
        return;
    }

    s_heap = multi_heap_register(s_arena, OGG_ARENA_SIZE);
    if (!s_heap) {
        ESP_LOGW(TAG, "multi_heap_register failed, using per-block PSRAM");
        heap_caps_free(s_arena);
        s_arena = NULL;
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGW(TAG, "arena mutex alloc failed (single-task use only)");
    }

    ESP_LOGI(TAG, "OGG arena ready: 1MB PSRAM (free PSRAM=%u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void *ogg_psram_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    void *p = NULL;
    arena_lock();
    if (s_heap) {
        p = multi_heap_malloc(s_heap, size);
        if (!p && !s_warned_exhausted) {
            s_warned_exhausted = true;
            ESP_LOGW(TAG, "arena exhausted at %u bytes, falling back to PSRAM heap",
                     (unsigned)size);
        }
    }
    arena_unlock();

    if (!p) {
        p = heap_caps_malloc(size, OGG_ARENA_CAPS);
    }
    return p;
}

void *ogg_psram_calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > (size_t)-1 / size) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = ogg_psram_malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *ogg_psram_realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return ogg_psram_malloc(size);
    }
    if (size == 0) {
        ogg_psram_free(ptr);
        return NULL;
    }

    if (!in_arena(ptr)) {
        return heap_caps_realloc(ptr, size, OGG_ARENA_CAPS);
    }

    void *out = NULL;
    size_t old_size = 0;

    arena_lock();
    old_size = multi_heap_get_allocated_size(s_heap, ptr);
    out = multi_heap_realloc(s_heap, ptr, size);
    arena_unlock();
    if (out) {
        return out;
    }

    /* 专用堆扩容失败：从全局 PSRAM 分配新块并拷贝，再归还旧块。
       失败时保留旧块，符合 realloc 语义。 */
    out = heap_caps_malloc(size, OGG_ARENA_CAPS);
    if (out) {
        memcpy(out, ptr, old_size < size ? old_size : size);
        arena_lock();
        multi_heap_free(s_heap, ptr);
        arena_unlock();
    }
    return out;
}

void ogg_psram_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    if (in_arena(ptr)) {
        arena_lock();
        multi_heap_free(s_heap, ptr);
        arena_unlock();
    } else {
        heap_caps_free(ptr);
    }
}
