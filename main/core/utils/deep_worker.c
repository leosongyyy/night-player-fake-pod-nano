#include "core/utils/deep_worker.h"

#include <assert.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "deep_worker";

/* 执行器任务参数
   ⚠ 栈 8KB（**不能再加大**）：本任务执行所有"深调用链"（息屏/亮屏的
   esp_pm_configure + AMOLED 重新初始化 + lv_refr_now 全屏重刷、关机
   停外设/渲染提示等），以及设置菜单"重新扫描"的"停现场"一段
   （player_controller_deinit → playlist_deinit → pldata_munmap）。
   注：2026-11-16 起 `pldata_munmap` 是空操作（播放列表数据搬到 SD 卡缓存文件，
   不再有 flash 分区映射要解除），本任务的栈放内部 RAM 是历史习惯，不再是
   "关缓存断言"的硬要求（详见 ideas_and_exps/PLDATA_ON_SDCARD.md）。
   ⚠ **栈 12KB（2026-10 TTF 迁移由 8KB 上调）**：本任务要跑 `lv_refr_now`
   全屏重刷（息屏唤醒）与关机提示渲染。TTF 时代这条链包含 FreeType 光栅化
   字形（光栅化器把渲染池放栈上），实测要吃掉近 12KB；**2026-11 已改回位图
   字体（NPF1），不再光栅化**，12KB 继续保留（未重新收紧，余量充足）。
   8KB 会在"息屏唤醒重刷 + 有未缓存字形"时溢出（栈在内部 RAM，溢出会踩着相邻
   内存）。⚠ 改到 12KB 后**必须确认任务创建成功**（下面有返回值检查与失败日志）。
   ⚠ **历史坑**：创建位置一旦挪到开机末期（内部 RAM 高度碎片化、最大连续块
   约 8.6KB）就会静默失败 → 每次深调用等满 5s 超时（按键息屏/亮屏延迟 5 秒）。
   所以本任务必须在 `app_main` 早期创建（config_store 之后、硬件初始化之前，
   此时内部 RAM 还有约 200KB 空闲/118KB 连续），并且检查 xTaskCreatePinnedToCore
   的返回值（失败时置 s_exec_task=NULL，deep_worker_call 快速失败 + 就地兜底）。
   ⚠ 优先级 6（沿用原关机任务参数）：高于 LVGL(2)/按键(4)，保证息屏/
   关机动作不被拖延；低于音频(8)，不会饿死解码。
   ⚠ 固定 Core0（与原关机任务一致；app_main 于 Core0 初始化全部驱动
   与中断，面板命令/全屏重刷在此核执行与现状等价）。 */
#define DEEP_WORKER_TASK_STACK  (12 * 1024)
#define DEEP_WORKER_TASK_PRIO   6
#define DEEP_WORKER_TASK_CORE   0

/* 单次深调用完成等待超时（ms）：深活最长约 250ms~1s（面板唤醒 120ms +
   重初始化 + 全屏重刷；关机停播等待最长 1s），5s 远超上限。关机最终
   由 PMU 断电：断电成功时调用方任务随整机一起消失，无需返回；只有关机
   失败（PMU 未响应）才会走回来，超时不可能触发。 */
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
static TaskHandle_t s_exec_task = NULL;   /* 执行器任务句柄（诊断用） */
static volatile bool s_exec_busy = false; /* 执行器是否正在跑 fn（诊断用） */

/* ---- 诊断辅助：超时时打印所有任务状态，定位"谁把执行器堵住了" ----
   ⚠ 依赖 CONFIG_FREERTOS_USE_TRACE_FACILITY（uxTaskGetSystemState）；
   release 配置未开启该选项，故条件编译掉（超时日志本身仍保留）。 */
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static const char *dw_task_state_str(eTaskState st)
{
    switch (st) {
    case eRunning:   return "RUN";
    case eReady:     return "READY";
    case eBlocked:   return "BLOCKED";
    case eSuspended: return "SUSP";
    case eDeleted:   return "DEL";
    default:         return "?";
    }
}

/* 打印所有任务的状态与优先级（诊断：谁把执行器堵住了） */
static void dw_dump_tasks(void)
{
    static TaskStatus_t arr[24];
    UBaseType_t n = uxTaskGetSystemState(arr, 24, NULL);
    char buf[384];
    size_t pos = 0;
    for (UBaseType_t i = 0; i < n && pos < sizeof(buf) - 24; i++) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " %s=%s/%u",
                                arr[i].pcTaskName,
                                dw_task_state_str(arr[i].eCurrentState),
                                (unsigned)arr[i].uxCurrentPriority);
    }
    ESP_LOGW(TAG, "tasks:%s", buf);
}
#else
static void dw_dump_tasks(void) { }
#endif

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
            /* 诊断日志：证明执行器确实取到并执行了 fn（历史上曾因任务创建
               失败而永远收不到请求，只有这两行能区分"没执行"与"执行慢"） */
            ESP_LOGI(TAG, "exec begin fn=%p", (void *)fn);
            s_exec_busy = true;
            fn(a);   /* 深链在大栈上执行 */
            s_exec_busy = false;
            ESP_LOGI(TAG, "exec end fn=%p", (void *)fn);
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

    /* ⚠ 必须检查返回值：内部 RAM 碎片化时 8KB 内部栈也可能分配失败，
       失败后若不报错，任务不存在 → 每次 deep_worker_call 都等满超时
       （历史事故：栈被改成 12KB 后静默失败，息屏/亮屏延迟 5 秒）。 */
    BaseType_t ret = xTaskCreatePinnedToCore(deep_worker_task, "deep_worker",
                                             DEEP_WORKER_TASK_STACK, NULL,
                                             DEEP_WORKER_TASK_PRIO,
                                             &s_exec_task,
                                             DEEP_WORKER_TASK_CORE);
    if (ret != pdPASS) {
        s_exec_task = NULL;
        ESP_LOGE(TAG, "create deep worker task FAILED (stack=%d B, internal "
                      "free=%u largest=%u) — deep calls will time out!",
                 DEEP_WORKER_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
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
    /* 执行器任务未创建（init 时内部 RAM 不足）：直接返回 false，
       由调用方就地兜底执行，不要白等 5 秒超时 */
    if (s_exec_task == NULL) {
        ESP_LOGE(TAG, "executor task missing, running inline");
        return false;
    }

    /* 串行化投递：持有 s_mux 直到本次执行完成，后续投递排队等待 */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_pending_fn = fn;
    s_pending_arg = arg;
    xSemaphoreTake(s_done, 0);                  /* 清可能残留的完成信号（防御） */
    xSemaphoreGive(s_req);
    const bool ok = (xSemaphoreTake(s_done, pdMS_TO_TICKS(DEEP_WORKER_TIMEOUT_MS)) == pdTRUE);
    /* ⚠ 只有确实收到完成信号才清空槽位：超时时执行器可能仍在跑 fn，
       此时清空槽位会让"下一次投递"的函数被这个正在跑的执行器读到并执行
       （错配 + 后续 done 信号被误消费）。 */
    if (ok) {
        s_pending_fn = NULL;
        s_pending_arg = NULL;
    }
    xSemaphoreGive(s_mux);
    if (!ok) {
        ESP_LOGE(TAG, "deep call timed out after %d ms (executor stuck? busy=%d)",
                 DEEP_WORKER_TIMEOUT_MS, (int)s_exec_busy);
        dw_dump_tasks();
    }
    return ok;
}
