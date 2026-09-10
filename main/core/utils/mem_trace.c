#include "core/utils/mem_trace.h"

#include "sdkconfig.h"

#if CONFIG_MEM_TRACE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "MEMTRACE";

/* 三组关心的内存能力：
   - INTERNAL|8BIT：内部 RAM 总空闲（含 IRAM 中可分配的字节区）
   - DMA：SPI/SDMMC 等外设可 DMA 访问的内部 RAM（绘制缓冲、SD 读缓冲的来源）
   - SPIRAM：PSRAM 堆（封面像素、.npd 文件缓冲等应落在这里） */
#define MEM_TRACE_INTERNAL  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define MEM_TRACE_DMA       (MALLOC_CAP_DMA)
#define MEM_TRACE_SPIRAM    (MALLOC_CAP_SPIRAM)

void mem_trace_caps(const char *name, unsigned int caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    ESP_LOGI(TAG, "  %-8s free=%6u largest=%6u blocks=%u/%u min_free=%u",
             name ? name : "-",
             (unsigned)info.total_free_bytes,
             (unsigned)info.largest_free_block,
             (unsigned)info.allocated_blocks,
             (unsigned)info.total_blocks,
             (unsigned)info.minimum_free_bytes);
}

void mem_trace_log(const char *stage)
{
    /* 单行紧凑输出：便于按阶段对比 free 与 blocks 的增量 */
    multi_heap_info_t in, dm, ps;
    heap_caps_get_info(&in, MEM_TRACE_INTERNAL);
    heap_caps_get_info(&dm, MEM_TRACE_DMA);
    heap_caps_get_info(&ps, MEM_TRACE_SPIRAM);

    ESP_LOGI(TAG, "%-22s int %6u/%6u(%u) | dma %6u/%6u | psram %8u",
             stage ? stage : "-",
             (unsigned)in.total_free_bytes, (unsigned)in.largest_free_block,
             (unsigned)in.allocated_blocks,
             (unsigned)dm.total_free_bytes, (unsigned)dm.largest_free_block,
             (unsigned)ps.total_free_bytes);
}

void mem_trace_ptr(const char *tag, const char *what, size_t bytes, const void *ptr)
{
    /* NULL 说明这次分配失败（调用点通常另有 ESP_LOGW 记录失败原因） */
    const char *where = (ptr == NULL) ? "FAIL"
                        : (esp_ptr_external_ram(ptr) ? "PSRAM" : "INTERNAL");
    ESP_LOGI(tag ? tag : TAG, "%s %u B -> %s",
             what ? what : "alloc", (unsigned)bytes, where);
}

#endif /* CONFIG_MEM_TRACE */
