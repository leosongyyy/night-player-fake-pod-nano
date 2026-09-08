#include "core/utils/task_psram.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

static const char *TAG = "task_psram";

BaseType_t task_create_psram_stack(TaskFunction_t task_func,
                                   const char *name,
                                   uint32_t stack_bytes,
                                   void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_handle,
                                   BaseType_t core_id)
{
    /* 1. 先试 PSRAM 栈（8MB 富余，不挤占内部 RAM） */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(task_func, name,
                                                     stack_bytes, arg,
                                                     priority, out_handle,
                                                     core_id,
                                                     MALLOC_CAP_SPIRAM);
    if (ret == pdPASS) {
        return ret;
    }

    /* 2. PSRAM 不可用/耗尽：回退内部 RAM（保持原有行为，不因省内存而丢功能） */
    ESP_LOGW(TAG, "task %s: PSRAM stack alloc failed, fallback to internal RAM",
             name);
    return xTaskCreatePinnedToCore(task_func, name, stack_bytes, arg,
                                   priority, out_handle, core_id);
}
