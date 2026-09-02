#include "core/utils/deep_worker.h"

#include <assert.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "deep_worker";

/* 执行器任务参数
   ⚠ 栈 8KB：本任务执行所有"深调用链"（息屏/亮屏的 esp_pm_configure +
   AMOLED 重新初始化 + lv_refr_now 全屏重刷、伪关机停外设/渲染提示等）。
   历史实测这些链在 8KB 栈上可安全运行——button/fake_shutdown 任务曾各自
   用 8KB 扛同类链（3KB/2KB 时曾爆栈）。实测栈高水位后可按需下调。
   ⚠ 优先级 6（沿用原伪关机任务参数）：高于 LVGL(2)/按键(4)，保证息屏/
   关机动作不被拖延；低于音频(8)，不会饿死解码。
   ⚠ 固定 Core0（与原伪关机任务一致；app_main 于 Core0 初始化全部驱动
   与中断，面板命令/全屏重刷在此核执行与现状等价）。 */
#define DEEP_WORKER_TASK_STACK  (8 * 1024)
#define DEEP_WORKER_TASK_PRIO   6
#define DEEP_WORKER_TASK_CORE   0

/* 单次深调用完成等待超时（ms）：深活最长约 250ms~1s（面板唤醒 120ms +
   重初始化 + 全屏重刷；伪关机停播等待最长 1s），5s 远超上限。伪关机最终
   deepsleep：芯片睡眠时调用方任务被冻结，无需返回，超时不可能触发。 */
#define DEEP_WORKER_TIMEOUT_MS  5000

/* 请求/完成信号量 + 投递互斥锁：
   - s_req   二值信号量：唤醒执行器执行一次投递的 fn；
   - s_done  二值信号量：fn 完成后通知投递者（同步等待）；
   - s_mux   互斥锁：投递者从"占槽位"到"等完成"期间一直持有，保证同一
     时刻只有一个投递请求在途（其余排队），fn/arg 槽位与 done 一一对应，
     不会因多调用方并发而错配。 */
static SemaphoreHandle_t s_req = NULL;
static SemaphoreHandle_t s_done = NULL;
static SemaphoreHandle_t s_mux = NULL;

static deep_worker_fn_t s_pending_fn = NULL;
static void *s_pending_arg = NULL;

/* 执行器任务：等待请求 → 执行 fn → 通知完成，循环往复 */
static void deep_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_req, portMAX_DELAY);   /* 等待投递 */
        deep_worker_fn_t fn = s_pending_fn;
        void *a = s_pending_arg;
        s_pending_fn = NULL;
        s_pending_arg = NULL;
        if (fn != NULL) {
            fn(a);   /* 深链在 8KB 大栈上执行 */
        }
        xSemaphoreGive(s_done);                 /* 通知投递者已完成 */
    }
}

void deep_worker_init(void)
{
    s_req = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    s_mux = xSemaphoreCreateMutex();
    assert(s_req != NULL && s_done != NULL && s_mux != NULL);

    xTaskCreatePinnedToCore(deep_worker_task, "deep_worker",
                            DEEP_WORKER_TASK_STACK, NULL,
                            DEEP_WORKER_TASK_PRIO, NULL,
                            DEEP_WORKER_TASK_CORE);
    ESP_LOGI(TAG, "deep worker ready: stack=%d B prio=%d core=%d",
             DEEP_WORKER_TASK_STACK, DEEP_WORKER_TASK_PRIO, DEEP_WORKER_TASK_CORE);
}

bool deep_worker_call(deep_worker_fn_t fn, void *arg)
{
    if (s_req == NULL || s_done == NULL || s_mux == NULL) {
        ESP_LOGE(TAG, "not initialized, call deep_worker_init() first");
        return false;
    }
    if (fn == NULL) {
        return false;
    }

    /* 串行化投递：持有 s_mux 直到本次执行完成，后续投递排队等待 */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_pending_fn = fn;
    s_pending_arg = arg;
    xSemaphoreTake(s_done, 0);                  /* 清可能残留的完成信号（防御） */
    xSemaphoreGive(s_req);
    const bool ok = (xSemaphoreTake(s_done, pdMS_TO_TICKS(DEEP_WORKER_TIMEOUT_MS)) == pdTRUE);
    s_pending_fn = NULL;
    s_pending_arg = NULL;
    xSemaphoreGive(s_mux);
    if (!ok) {
        ESP_LOGE(TAG, "deep call timed out after %d ms (executor stuck?)",
                 DEEP_WORKER_TIMEOUT_MS);
    }
    return ok;
}
