#include "performance_monitor.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/task_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdio.h>

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
   读到 80，用于验证"息屏降频生效"（见 core/utils/power_service）。 */
static volatile int s_freq_isr_mhz = 0;

/* esp_timer ISR 频率采样回调（中断上下文，只写一个 int，不做重活）
 *
 * ⚠⚠ **必须 IRAM_ATTR（真实崩溃修复，2026-11）**：本定时器用
 * `.dispatch_method = ESP_TIMER_ISR` 创建，回调是**在 systimer 中断里直接执行**
 * 的（不是 esp_timer 任务上下文）。而 systimer 中断注册时带
 * `ESP_INTR_FLAG_IRAM`（IDF `esp_timer_impl_systimer.c`），意味着
 * `spi_flash_disable_interrupts_caches_and_other_cpu()` 里的
 * `esp_intr_noniram_disable()`（只屏蔽**非 IRAM** 中断）**屏蔽不掉它**——
 * 任何 flash 操作（NVS 提交、pldata 擦写…）关外部内存缓存期间它照样能触发。
 * 此时回调若还在 flash 里（不加 IRAM_ATTR 就是），取指必然失败。
 *
 * 实测崩溃现场（配置槽位切换 → NVS 写 flash 期间命中 500ms 采样点）：
 *   Core 0 panic'ed (Cache error) / Cache disabled but cached memory region accessed
 *   PC: cpu_freq_isr_cb  ← flash 地址
 *   栈: lvgl_port_task → ... → config_store_switch_slot → nvs_set_u8
 *       → esp_flash_write →（关缓存）→ timer_alarm_isr → cpu_freq_isr_cb
 * ⚠ 触发概率与"NVS 写密度"成正比：写 flash 的窗口约 1~10ms，采样周期 500ms，
 *   单次写撞上的概率约 0.2%~2%；本回调只读一个寄存器、放 IRAM 零代价。
 * ⚠ 以后再加 `ESP_TIMER_ISR` 定时器，回调**一律**要 IRAM_ATTR，且只能调用
 *   IRAM 安全函数（`esp_clk_cpu_freq()` 本身是 IRAM_ATTR，可安全调用）。 */
static void IRAM_ATTR cpu_freq_isr_cb(void *arg)
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

/* 关键任务栈高水位汇总（低频打印，验证深链委托/缩栈安全性）：
   每 20s 打印一次所有目标任务的"最小剩余栈"（uxTaskGetStackHighWaterMark，
   自启动以来峰值用栈的余量；本 port StackType_t=uint8_t，返回值即字节）。
   - 值越小 → 峰值用栈越接近分配值（接近爆栈）；
   - 任务不存在（未创建，如非播放态的 mp3_play；或已删除）时跳过。
   用途：确认 deep_worker 接手深链后 button/power_service 等任务的高富余，
   作为后续缩栈的依据；同时监控 deep_worker 自身 8KB 栈的余量。 */
static void mem_report_stack_hwm(void)
{
    static const char *names[] = {
        "deep_worker", "button_svc", "power_svc", "LVGL", "player_ctrl",
        "sleep_timer", "mem_monitor", "cpu_coll0", "cpu_coll1",
        "mp3_play", "mp3_out", "cover_load", "hw_selftest", "pmu_irq",
        "touch_init", "boot_scan", "rescan",
    };
    char buf[320];
    size_t pos = 0;
    for (size_t i = 0;
         i < sizeof(names) / sizeof(names[0]) && pos < sizeof(buf) - 24; i++) {
        const TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h == NULL) {
            continue;   /* 未创建/已删除 */
        }
        const UBaseType_t hwm = uxTaskGetStackHighWaterMark(h);
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " %s=%u",
                                names[i], (unsigned)hwm);
    }
    ESP_LOGI("MEM", "stk:%s", buf);
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
    /* 采集任务栈放 PSRAM（内部 RAM 开机末期仅剩 4~5KB，1KB 内部栈也曾起不来）；
       本任务只读空闲计数器，栈访问偶发缓存未命中无影响 */
    task_create_psram_stack(cpu_collector_task, "cpu_coll0",
                            CPU_COLLECTOR_STACK, (void *)&s_core0_usage,
                            CPU_COLLECTOR_PRIO, &dummy, 0);
    task_create_psram_stack(cpu_collector_task, "cpu_coll1",
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
    /* 帧率统计：LVGL 每次完成刷屏都会给 lvgl_port 的计数 +1，这里按采样间隔
       取差值。静止时 LVGL 不刷新，fps=0 属正常；滚动/动画时才是有意义的读数。 */
    uint32_t last_refr_cnt = lvgl_port_get_refr_count();
    int64_t  last_refr_us  = esp_timer_get_time();
    uint32_t loop_cnt = 0;
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

        /* 帧率 = 本次间隔内完成的刷新帧数 / 间隔时长（无符号差值，回绕安全） */
        const uint32_t refr_now = lvgl_port_get_refr_count();
        const int64_t  now_us   = esp_timer_get_time();
        uint32_t fps = 0;
        if (now_us > last_refr_us) {
            fps = (uint32_t)(((uint64_t)(uint32_t)(refr_now - last_refr_cnt) * 1000000ULL) /
                             (uint64_t)(now_us - last_refr_us));
        }
        last_refr_cnt = refr_now;
        last_refr_us  = now_us;

        ESP_LOGI("MEM", "heap=%u (%c%u)  iram=%u (%c%u)  cpu0=%u%% cpu1=%u%%  freq=%dMHz(idle:%dMHz)  fps=%u",
                 (unsigned)heap_now, hd >= 0 ? '+' : '-', (unsigned)(hd >= 0 ? hd : -hd),
                 (unsigned)internal_now, id >= 0 ? '+' : '-', (unsigned)(id >= 0 ? id : -id),
                 (unsigned)s_core0_usage, (unsigned)s_core1_usage, cpu_mhz,
                 (int)s_freq_isr_mhz, (unsigned)fps);

        last_heap = heap_now;
        last_internal = internal_now;

        /* 每 10 轮（约 20s）打印一次任务栈高水位（验证缩栈安全性） */
        if ((++loop_cnt % 10) == 0) {
            mem_report_stack_hwm();
        }
    }
}

void startPerformanceMonitor(void)
{
    /* 栈放 PSRAM（内部 RAM 紧张时 4KB 内部栈会创建失败，表现为"监视任务起不来"）；
       本任务每 2s 醒一次只做采样打印，PSRAM 栈无性能影响 */
    if (task_create_psram_stack(mem_monitor_task, "mem_monitor",
                                MEM_MONITOR_TASK_STACK, NULL,
                                MEM_MONITOR_TASK_PRIO, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE("MEM", "create mem_monitor task failed (internal free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
