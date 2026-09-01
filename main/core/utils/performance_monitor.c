#include "performance_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include <stdint.h>

#define MEM_MONITOR_TASK_STACK 4096
#define MEM_MONITOR_TASK_PRIO  2
#define MEM_MONITOR_INTERVAL_MS 2000
#define CPU_COLLECTOR_STACK    1024
#define CPU_COLLECTOR_PRIO     1
#define CPU_COLLECTOR_INTERVAL 1000

/* 每个核心的CPU使用率（百分比），由各自核心上的采集任务计算更新 */
static volatile uint32_t s_core0_usage = 0;
static volatile uint32_t s_core1_usage = 0;

/* ISR 采样的 CPU 频率（MHz）：由 esp_timer ISR 定时器周期性读取。
   ⚠ 为什么需要 ISR 采样：esp_pm DFS 下任何任务代码执行期间（该核持有
   "rtos" CPU 频率锁）频率都是 240MHz，空闲期才降到 80MHz——任务上下文
   （mem_monitor 打印）永远读不到空闲频率。esp_timer ISR 在 tick 中断之外
   触发、不会重新上锁，因此 ISR 采样能反映空闲期的真实频率：
   亮屏（持有 screen 锁）时恒为 240；息屏释放锁后，采样点落在空闲期即
   读到 80，用于验证"息屏降频生效"（见 core/utils/screen_sleep）。 */
static volatile int s_freq_isr_mhz = 0;

/* esp_timer ISR 频率采样回调（中断上下文，只写一个 int，不做重活） */
static void cpu_freq_isr_cb(void *arg)
{
    (void)arg;
    s_freq_isr_mhz = esp_clk_cpu_freq() / 1000000;
}

/* CPU采集任务（通过pvParameters传入目标volatile变量的地址） */
static void cpu_collector_task(void *pvParameters)
{
    volatile uint32_t *pUsage = (volatile uint32_t *)pvParameters;
    uint32_t last_idle = ulTaskGetIdleRunTimeCounter();
    uint32_t last_time = (uint32_t)esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CPU_COLLECTOR_INTERVAL));
        uint32_t now_time = (uint32_t)esp_timer_get_time();
        uint32_t now_idle = ulTaskGetIdleRunTimeCounter();
        uint32_t elapsed = now_time - last_time;
        uint32_t idle_delta = now_idle - last_idle;
        if (elapsed > 0 && idle_delta <= elapsed) {
            *pUsage = 100 - (idle_delta * 100 / elapsed);
        }
        last_idle = now_idle;
        last_time = now_time;
    }
}

static void mem_monitor_task(void *pvParameters)
{
    /*
     * 双核CPU监视方案：
     * - cpu_collector_task 在每个核心上采集空闲计数器并计算利用率
     * - 通过 pvParameters 传入目标 volatile 变量地址
     * - 本任务只负责内存监视和汇总展示
     */
    TaskHandle_t dummy;
    xTaskCreatePinnedToCore(cpu_collector_task, "cpu_coll0",
                            CPU_COLLECTOR_STACK, (void *)&s_core0_usage,
                            CPU_COLLECTOR_PRIO, &dummy, 0);
    xTaskCreatePinnedToCore(cpu_collector_task, "cpu_coll1",
                            CPU_COLLECTOR_STACK, (void *)&s_core1_usage,
                            CPU_COLLECTOR_PRIO, &dummy, 1);

    /* esp_timer ISR 定时器：每 500ms 在中断上下文采样一次 CPU 频率
       （空闲期降频后的值，见 cpu_freq_isr_cb 注释） */
    const esp_timer_create_args_t freq_timer_args = {
        .callback = cpu_freq_isr_cb,
        .name = "freq_sampler",
        .dispatch_method = ESP_TIMER_ISR,
    };
    esp_timer_handle_t freq_timer = NULL;
    if (esp_timer_create(&freq_timer_args, &freq_timer) == ESP_OK) {
        esp_timer_start_periodic(freq_timer, 500 * 1000);
    }

    /* 等待采集任务完成首次采样计算 */
    vTaskDelay(pdMS_TO_TICKS(CPU_COLLECTOR_INTERVAL + 200));

    size_t last_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t last_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_INTERVAL_MS));

        /* 内存监视 */
        size_t heap_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t internal_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int hd = (int)(heap_now - last_heap);
        int id = (int)(internal_now - last_internal);

        /* 当前 CPU 频率（MHz）：esp_pm 动态调频下随负载/息屏变化。
           任务内读数恒为 240（任务运行即持 CPU 频率锁）；括号内 idle 值
           为 ISR 采样的空闲期频率：亮屏恒 240，息屏释放锁后空闲期降到 80，
           用于验证"息屏降频生效"。 */
        int cpu_mhz = esp_clk_cpu_freq() / 1000000;

        ESP_LOGI("MEM", "heap=%u (%c%u)  iram=%u (%c%u)  cpu0=%u%% cpu1=%u%%  freq=%dMHz(idle:%dMHz)",
                 (unsigned)heap_now, hd >= 0 ? '+' : '-', (unsigned)(hd >= 0 ? hd : -hd),
                 (unsigned)internal_now, id >= 0 ? '+' : '-', (unsigned)(id >= 0 ? id : -id),
                 (unsigned)s_core0_usage, (unsigned)s_core1_usage, cpu_mhz,
                 (int)s_freq_isr_mhz);

        last_heap = heap_now;
        last_internal = internal_now;
    }
}

void startPerformanceMonitor(void)
{
    xTaskCreate(mem_monitor_task, "mem_monitor",
                MEM_MONITOR_TASK_STACK, NULL,
                MEM_MONITOR_TASK_PRIO, NULL);
}
