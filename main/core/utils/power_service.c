#include "core/utils/power_service.h"

#include <assert.h>

#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/task_psram.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "power_service";

/* 后台守护任务检查周期（ms）：1 秒粒度足够（自动息屏超时配置最小档 15 秒，
   低电关机与暂停关机按秒累计）。 */
#define POWER_SVC_CHECK_MS  1000

/* 守护任务参数：Core1 低优先级后台任务，每秒醒来一次，几乎不占 CPU。
   放 Core1（与 LVGL/按键分开），优先级最低，不影响解码/输出/UI。
   ⚠ 栈 3KB：息屏/亮屏的深调用链（esp_pm_configure + AMOLED 休眠/唤醒 +
   lv_refr_now 全屏重刷）已委托给 deep_worker 大栈任务（见下与
   core/utils/deep_worker.h），本任务自身只做超时检查与浅投递。实测自动
   息屏投递路径（power_svc → deep_worker_call 等待完成）栈峰值约
   1.9KB——曾缩到 2KB 实测仅剩 116B 余量（过紧），3KB 留约 1.1KB 余量。
   历史 2KB 栈爆栈是直接内跑 esp_pm_configure 深链所致，勿回到在任务内
   直接执行深链的写法。 */
#define POWER_SVC_TASK_STACK (3 * 1024)
#define POWER_SVC_TASK_PRIO  1
#define POWER_SVC_TASK_CORE  1

/* 固定频率（MHz）：亮屏锁 320MHz（S31 CPLL 最高档）；息屏统一 80MHz
   （OGG 换 Tremor 定点解码后，播放与暂停/停止不再区分频点）。
   ⚠ 不用 DFS 动态调频（实测息屏释放频率锁后空闲期降频不明显），改为
   esp_pm_configure 把最大/最小频率都设成同一值强制固定。
   ⚠ S31 的 CPU 频点由 CPLL/BSPLL 分频得到，80MHz 若不被该芯片支持，
   esp_pm_configure 会返回错误（本模块只告警、保持原频率），息屏功能不受影响。 */
#define PM_CPU_FREQ_ON_MHZ  320
#define PM_CPU_FREQ_OFF_MHZ 80
/* 蓝牙音频播放时的熄屏频率：重采样 + EQ + SBC 编码在 80MHz 下余量不足，
 * 160MHz 实测足够（SBC 编码由协议栈内部完成，重采样是主要开销） */
#define PM_CPU_FREQ_OFF_BT_MHZ 160

/* 低电自动关机（S31，2026-11 接回）：电量 < 5% 且**未在充电**持续 60 秒 →
   与上键长按同一条 PMU 真关机流程。
   · 判据用 AXP2101 电量计百分比（battery_read_info 的融合值），与开机拒启
     （boot_flow 的 2%）同一数据源，运行中阈值更保守；
   · 充电中 / VBUS 在位时不累计，避免边充边用的低电量机器被误关机；
   · 读数不可用（percent == 0xFF）或 PMU 未就绪时不动作（宁可不关，不误关）。 */
#define LOW_BAT_SHUTDOWN_PERCENT  5
#define LOW_BAT_SHUTDOWN_SEC      60

/* 低电条件已持续的秒数：条件一旦断开立即清零（要求"持续"而非瞬时抖动） */
static uint32_t s_low_bat_sec = 0;

/* S31 平台说明：S3 的"息屏暂停超时自动关机"依赖 deepsleep 伪关机，已随平台
   迁移移除；低电策略 2026-11 按 AXP2101 的电量百分比重新接回（见上方
   LOW_BAT_* 常量，与开机拒启的 2% 同一数据源、阈值更保守）。
   关机一律是 AXP2101 真断电（`fake_shutdown` 写 `0x10[0]=1`）。 */

/* 息屏状态与状态切换互斥锁：screen_off/screen_on 可能来自不同任务（按键
   任务/守护任务），用互斥锁保护状态机与"固定频率、挂起/恢复 LVGL、改亮度"
   的先后顺序，避免并发交错导致屏幕亮度与 LVGL 状态不一致。 */
static bool s_screen_off = false;
static SemaphoreHandle_t s_mux = NULL;

/* 最近一次用户活动时刻（esp_timer，us）：按键/触摸时刷新；
   守护任务读取。volatile 保证跨任务可见性（单字节/64 位写原子）。 */
static volatile int64_t s_last_activity_us = 0;

/* "保持亮屏"抑制标志（网络对时等长流程）：true 时跳过无操作自动息屏检查。
   只影响自动息屏，手动/按键息屏照常。 */
static volatile bool s_hold_awake = false;

/* 播放线程的性能需求位掩码（见 power_perf_req_t）：非 0 时熄屏用 160MHz 而不是 80MHz。
   s_quiet=true 表示处于静音伪关机（安静模式）：强制 80MHz，且跳过低电/自动息屏策略。 */
static volatile uint32_t s_perf_mask = 0;
static volatile bool s_quiet = false;

static esp_err_t power_service_set_cpu_freq(int mhz);

/* 目标频率：安静模式 80；息屏时按性能需求 80/160；亮屏 320 */
static int power_service_target_freq_mhz(void)
{
    if (s_quiet) {
        return PM_CPU_FREQ_OFF_MHZ;
    }
    if (s_screen_off) {
        return (s_perf_mask != 0) ? PM_CPU_FREQ_OFF_BT_MHZ : PM_CPU_FREQ_OFF_MHZ;
    }
    return PM_CPU_FREQ_ON_MHZ;
}

/* 按当前状态应用固定频率（失败只告警，保持原频率） */
static void power_service_apply_freq(void)
{
    int mhz = power_service_target_freq_mhz();
    if (power_service_set_cpu_freq(mhz) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(%dMHz) failed, freq unchanged", mhz);
    }
}

/* 固定 CPU 频率（esp_pm_configure 强制 max=min=目标值）：
   返回 ESP_OK 成功；CONFIG_PM_ENABLE 未开启时返回 ESP_ERR_NOT_SUPPORTED，
   调用方降级（不调频，其余息屏功能正常）。 */
static esp_err_t power_service_set_cpu_freq(int mhz)
{
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,   /* 息屏不是睡眠：只固定频率，不 light sleep */
    };
    return esp_pm_configure(&pm_cfg);
}

/* 触摸活动回调（LVGL 任务上下文，已持锁）：按下即视为一次用户活动 */
static void power_service_touch_cb(bool pressed, uint16_t tx, uint16_t ty, void *user_ctx)
{
    (void)tx;
    (void)ty;
    (void)user_ctx;
    if (pressed) {
        power_service_note_activity();
    }
}

/* ------------------------------------------------------------------ */

/* 守护任务：亮屏且距最近活动超过配置时长 → 自动息屏。
   （S3 时代的暂停/低电自动伪关机策略已随 S31 迁移移除，见上方说明） */
static void power_service_task(void *arg)
{
    (void)arg;
    battery_info_t binfo;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POWER_SVC_CHECK_MS));

        /* 电池健康保护（4.0V 软件限充）：每秒检查一次，达到 4.0V 停充、
           降到 3.9V 恢复；保护关闭时几乎零开销（快速返回）。 */
        battery_health_protection_tick();

        /* 低电自动关机：电量 < 5% 且未充电**持续** 60s → PMU 真关机。
           条件一旦断开（插上充电器/电量回升/读数不可用）立即清零，
           保证是"持续低电"才关，不被瞬时抖动误触发。
           ⚠ 息屏播放中同样生效：电池过放比"夜里被关机"更不可接受。 */
        const bool low = !s_quiet
                         && (battery_read_info(&binfo) == ESP_OK)
                         && binfo.available && binfo.percent != 0xFF
                         && binfo.percent < LOW_BAT_SHUTDOWN_PERCENT
                         && !binfo.vbus_present && !binfo.charging;
        if (low) {
            s_low_bat_sec++;
            if (s_low_bat_sec >= LOW_BAT_SHUTDOWN_SEC) {
                ESP_LOGW(TAG, "battery %u%% and not charging for %u s, shutting down",
                         (unsigned)binfo.percent, (unsigned)s_low_bat_sec);
                s_low_bat_sec = 0;
                fake_shutdown_enter();
            }
        } else {
            s_low_bat_sec = 0;
        }

        /* 息屏超时：仅亮屏时检查（息屏期间只允许按键唤醒）；被 hold_awake
           抑制时（网络对时/长流程）跳过，避免进度界面中途黑屏 */
        if (!s_screen_off && !s_hold_awake && !s_quiet) {
            uint32_t timeout_s = config_store_get_screen_off_s();
            if (timeout_s != 0
                    && esp_timer_get_time() - s_last_activity_us
                       >= (int64_t)timeout_s * 1000000LL) {
                ESP_LOGI(TAG, "no activity for %u s, auto screen off",
                         (unsigned)timeout_s);
                power_service_screen_off();
            }
        }
    }
}

void power_service_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }

    /* 1. 初始固定 320MHz（亮屏状态）。
          ⚠ esp_pm_configure 依赖 CONFIG_PM_ENABLE=y（见 sdkconfig.defaults）；
未开启时返回 ESP_ERR_NOT_SUPPORTED，此处降级：不调频，其余功能正常。 */
    power_service_apply_freq();
    ESP_LOGI(TAG, "esp_pm ready: screen on=%d MHz, screen off=%d MHz "
                  "(+%d MHz when BT audio), fixed",
             PM_CPU_FREQ_ON_MHZ, PM_CPU_FREQ_OFF_MHZ, PM_CPU_FREQ_OFF_BT_MHZ);

    /* 2. 状态互斥锁 */
    s_mux = xSemaphoreCreateMutex();
    assert(s_mux);

    /* 3. 活动计时基准：从初始化时刻起算（开机后无操作超时从此刻开始） */
    s_last_activity_us = esp_timer_get_time();

    /* 4. 订阅触摸活动：按下算一次活动（复用 LVGL indev 轮询，无额外轮询） */
    lvgl_port_register_touch_cb(power_service_touch_cb, NULL);

    /* 5. 守护任务（自动息屏）
       ⚠ 栈放 **PSRAM**（task_create_psram_stack，失败自动回退内部 RAM）：
       开机末期内部 RAM 已接近耗尽（实测内部空闲仅约 4.5KB、DMA 约 2KB），
       3KB 栈的内部 RAM 分配会失败（"create power service task failed"），
       导致自动息屏/自动关机全部失效。本任务每秒醒来一次、只做超时判断与
       浅投递（深链已交给 deep_worker），PSRAM 栈的性能影响可忽略。 */
    if (task_create_psram_stack(power_service_task, "power_svc",
                                POWER_SVC_TASK_STACK, NULL,
                                POWER_SVC_TASK_PRIO, NULL,
                                POWER_SVC_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create power service task failed (internal free=%u, "
                      "spiram free=%u, stack=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)POWER_SVC_TASK_STACK);
    }

    inited = true;
    ESP_LOGI(TAG, "power service ready (auto screen off after %u s)",
             (unsigned)config_store_get_screen_off_s());
}

/* ============ 息屏/亮屏"重活"（由 deep_worker 大栈任务执行） ============
   原 screen_off/screen_on 的执行体：esp_pm_configure（跨核 CCOMPARE
   同步，历史 2KB 栈实测爆栈）、AMOLED 休眠/唤醒（display_sleep_exit 含
   120ms 等待 + esp_lcd_panel_init 重初始化深链）、lv_refr_now 全屏同步重刷、
   lvgl_port_suspend/resume 等，调用链很深。集中到 deep_worker 任务（8KB 栈）
   执行后，任何调用方（守护任务/按键任务）只需浅投递 + 等待完成；
   deep_worker_call 失败（未初始化/卡死超时）时由调用方就地兜底执行，
   保证屏幕与 LVGL 状态始终一致。 */

/* 息屏重活：AMOLED 休眠 → 挂起 LVGL → 固定 80/160MHz（蓝牙音频时 160） */
static void power_service_do_screen_off(void *arg)
{
    /* 0. 息屏前把"延迟落盘"的亮度/音量写进 NVS：LVGL 任务马上要被挂起，
          而拖动亮度的落盘正是挂在 LVGL 的定时器上（松手也会 flush，但手势
          被中断时会漏）。arg != NULL 表示"经 deep_worker（内部 RAM 栈）执行"，
          只有这个上下文才能写 flash——deep_worker 不可用时调用方会就地兜底
          （power_svc 的栈在 PSRAM，写 flash 会命中 IDF 的关缓存断言），
          故兜底路径传 NULL、跳过本步。 */
    if (arg != NULL) {
        config_store_flush_pending(true);
    }

    /* 1. AMOLED 休眠（DISPOFF + SLEEP IN）：驱动 IC 停止 DC/DC/振荡器/扫描，
           显示 RAM 保留——这是息屏真正的低功耗态（仅亮度 0 时驱动 IC 仍
           满负荷运行，曾是息屏电流大头）。⚠ 命令与 LVGL 刷屏共用 SPI 总线，
           必须持 LVGL 锁。 */
    if (lvgl_port_lock(-1)) {
        display_sleep_enter();
        lvgl_port_unlock();
    }

    /* 2. 挂起 LVGL：停止 tick（LVGL 时间冻结）+ 挂起任务（停止刷屏与触摸轮询）。
          AMOLED 为存储型显示，休眠后屏幕全黑且保持最后帧（RAM 保留），
          唤醒时重新初始化并全屏重刷。 */
    lvgl_port_suspend();

/* 3. 固定 CPU 频率：普通 80MHz，蓝牙音频流进行中 160MHz（性能需求） */
    power_service_apply_freq();

    ESP_LOGI(TAG, "screen off: AMOLED sleep in, LVGL suspended, CPU fixed %d MHz",
             power_service_target_freq_mhz());
}

/* 亮屏重活：320MHz → 持锁唤醒 AMOLED + 全屏重刷 + 恢复亮度 → resume LVGL */
static void power_service_do_screen_on(void *arg)
{
    (void)arg;
    /* 1. 恢复 CPU 频率 320MHz（先升频再恢复显示，避免显示恢复瞬间系统还在低频） */
    power_service_apply_freq();

    /* 2. 唤醒 AMOLED（SLEEP OUT + 重发完整初始化）并恢复显示：
          ⚠ 必须先于 LVGL resume——休眠中/刚唤醒的面板不能接收刷屏数据；
          ⚠ 实测该面板 SLEEP OUT 后需重新初始化（仅 0x11+DISPON 不亮屏），
          重初始化会重置显示 RAM → 全屏重刷（invalidate + 同步 lv_refr_now，
          此时亮度仍 0 全黑、无闪烁窗口），刷完再恢复用户亮度（与开机
          "全黑启动防白屏"的点亮顺序一致）。全程持 LVGL 锁。 */
    if (lvgl_port_lock(-1)) {
        display_sleep_exit();
        lv_obj_invalidate(lv_scr_act());   /* 面板 RAM 已重置：整屏标记脏区 */
        lv_refr_now(NULL);                 /* 同步刷出整屏（黑屏中完成） */
        display_set_brightness(config_store_get_brightness());  /* 刷完再点亮 */
        lvgl_port_unlock();
    }

    /* 3. 恢复 LVGL 任务与 tick（LVGL 时间从挂起时刻继续，无跳变） */
    lvgl_port_resume();

    ESP_LOGI(TAG, "screen on: LVGL resumed, brightness %u restored, CPU fixed %d MHz",
             (unsigned)config_store_get_brightness(), PM_CPU_FREQ_ON_MHZ);
}

void power_service_screen_off(void)
{
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_screen_off) {
        xSemaphoreGive(s_mux);
        return;
    }
    s_screen_off = true;

    /* 状态机翻转后，把深链投递给 deep_worker 大栈任务执行并等待完成
       （本调用方只做浅投递，不扛深调用链）；失败（executor 未初始化/
       卡死超时）时就地兜底执行，保证屏幕与 LVGL 状态一致。
       ⚠ s_mux 全程持有：息屏动作同步完成后才返回，快速连按由 s_mux
       串行化（见 SCREEN_SLEEP_AMOLED_OPTIMIZATION.md 第 5 节时序）。 */
    if (!deep_worker_call(power_service_do_screen_off, (void *)1)) {
        /* 兜底就地执行，传 NULL：本任务栈在 PSRAM，绝不能在里面写 flash */
        power_service_do_screen_off(NULL);
    }
    xSemaphoreGive(s_mux);
}

void power_service_screen_on(void)
{
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_screen_off) {
        xSemaphoreGive(s_mux);
        return;
    }
    s_screen_off = false;

    /* 同 screen_off：深链交给 deep_worker 执行（含全屏重刷），失败就地兜底 */
    if (!deep_worker_call(power_service_do_screen_on, NULL)) {
        power_service_do_screen_on(NULL);
    }

    /* 重置活动计时：刚亮屏，避免立刻再次自动息屏 */
    s_last_activity_us = esp_timer_get_time();

    xSemaphoreGive(s_mux);
}

void power_service_screen_toggle(void)
{
    if (s_screen_off) {
        power_service_screen_on();
    } else {
        power_service_screen_off();
    }
}

bool power_service_is_screen_off(void)
{
    return s_screen_off;
}

void power_service_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

void power_service_set_hold_awake(bool hold)
{
    if (hold) {
        /* 置位时同时刷新计时基准：否则守护任务下次醒来仍按旧时间判超时 */
        s_last_activity_us = esp_timer_get_time();
    }
    s_hold_awake = hold;
}

uint32_t power_service_get_idle_sec(void)
{
    int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
    if (idle_us < 0) {
        idle_us = 0;
    }
    return (uint32_t)(idle_us / 1000000);
}

void power_service_perf_set(power_perf_req_t req, bool on)
{
    if (req < 0 || req >= POWER_PERF_COUNT) {
        return;
    }
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    const uint32_t bit = 1u << (unsigned)req;
    uint32_t before;
    taskENTER_CRITICAL(&mux);
    before = s_perf_mask;
    if (on) {
        s_perf_mask |= bit;
    } else {
        s_perf_mask &= ~bit;
    }
    uint32_t after = s_perf_mask;
    taskEXIT_CRITICAL(&mux);

    if (before != after && s_screen_off) {
        /* 熄屏播放中流开始/结束：立即调频（80 ↔ 160MHz） */
        power_service_apply_freq();
        ESP_LOGI(TAG, "perf req %d -> %d, screen-off freq now %d MHz",
                 (int)req, (int)on, power_service_target_freq_mhz());
    }
}

void power_service_set_quiet_mode(bool on)
{
    if (s_quiet == on) {
        return;
    }
    s_quiet = on;
    power_service_apply_freq();
    ESP_LOGI(TAG, "quiet mode %s (freq fixed %d MHz)", on ? "on" : "off",
             power_service_target_freq_mhz());
}
