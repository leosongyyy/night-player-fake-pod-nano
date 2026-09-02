#include "core/utils/screen_sleep.h"

#include <assert.h>

#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/fake_shutdown.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "screen_sleep";

/* 无操作超时检查周期（ms）：1 秒粒度足够（超时配置最小档 15 秒） */
#define SCREEN_SLEEP_CHECK_MS  1000

/* 超时检测任务参数：Core1 低优先级后台任务，每秒醒来一次，几乎不占 CPU。
   放 Core1（与 LVGL/按键分开），优先级最低，不影响解码/输出/UI。
   ⚠ 栈 3KB：息屏/亮屏的深调用链（esp_pm_configure + AMOLED 休眠/唤醒 +
   lv_refr_now 全屏重刷）已委托给 deep_worker 大栈任务（见下与
   core/utils/deep_worker.h），本任务自身只做超时检查与浅投递。实测自动
   息屏投递路径（screen_sleep_task → deep_worker_call 等待完成）栈峰值约
   1.9KB——曾缩到 2KB 实测仅剩 116B 余量（过紧），3KB 留约 1.1KB 余量。
   历史 2KB 栈爆栈是直接内跑 esp_pm_configure 深链所致，勿回到在任务内
   直接执行深链的写法。 */
#define SCREEN_SLEEP_TASK_STACK (3 * 1024)
#define SCREEN_SLEEP_TASK_PRIO  1
#define SCREEN_SLEEP_TASK_CORE  1

/* 固定频率（MHz）：亮屏锁 240MHz；息屏直接锁 80MHz。
   ⚠ 不用 DFS 动态调频（实测息屏释放频率锁后空闲期降频不明显），改为
   esp_pm_configure 把最大/最小频率都设成同一值强制固定：
   - 亮屏：240/240 → CPU 恒 240MHz；
   - 息屏：80/80 → CPU 恒 80MHz（省电；ESP32-S3 的 APB 恒 80MHz，
     I2S/SPI 时钟不受影响，但高采样率/高码率 MP3 解码在 80MHz 下可能
     更吃力，若实测卡顿可考虑改锁 160MHz）。 */
#define PM_CPU_FREQ_ON_MHZ  240
#define PM_CPU_FREQ_OFF_MHZ 80

/* 暂停超时自动伪关机（分钟）：暂停状态持续超过该时长自动伪关机省电。
   经 Kconfig CONFIG_PAUSE_AUTO_SHUTDOWN_MIN 配置（默认 10 分钟，0=关闭）。 */
#ifdef CONFIG_PAUSE_AUTO_SHUTDOWN_MIN
#define PAUSE_SHUTDOWN_MIN  CONFIG_PAUSE_AUTO_SHUTDOWN_MIN
#else
#define PAUSE_SHUTDOWN_MIN  10
#endif

/* 息屏状态与状态切换互斥锁：enter/exit 可能来自不同任务（按键任务/超时
   检测任务），用互斥锁保护状态机与"固定频率、挂起/恢复 LVGL、改亮度"
   的先后顺序，避免并发交错导致屏幕亮度与 LVGL 状态不一致。 */
static bool s_sleeping = false;
static SemaphoreHandle_t s_mux = NULL;

/* 最近一次用户活动时刻（esp_timer，us）：按键/触摸时刷新；
   超时检测任务读取。volatile 保证跨任务可见性（单字节/64 位写原子）。 */
static volatile int64_t s_last_activity_us = 0;

/* 固定 CPU 频率（esp_pm_configure 强制 max=min=目标值）：
   返回 ESP_OK 成功；CONFIG_PM_ENABLE 未开启时返回 ESP_ERR_NOT_SUPPORTED，
   调用方降级（不调频，其余息屏功能正常）。 */
static esp_err_t screen_sleep_set_cpu_freq(int mhz)
{
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,   /* 息屏不是睡眠：只固定频率，不 light sleep */
    };
    return esp_pm_configure(&pm_cfg);
}

/* 触摸活动回调（LVGL 任务上下文，已持锁）：按下即视为一次用户活动 */
static void screen_sleep_touch_cb(bool pressed, uint16_t tx, uint16_t ty, void *user_ctx)
{
    (void)tx;
    (void)ty;
    (void)user_ctx;
    if (pressed) {
        screen_sleep_note_activity();
    }
}

/* 暂停超时自动伪关机（省电）：**仅统计"息屏 + 暂停"**——亮屏时用户在看
   屏幕随时会操作，暂停时长不累计；息屏且暂停持续超过 PAUSE_SHUTDOWN_MIN
   分钟 → 普通伪关机（deepsleep）。
   伪关机前 player_controller_save_last_position 会记录播放状态
   （was_playing=false），开机恢复暂停态不自动播放。 */
static bool s_prev_paused = false;      /* 上一轮检测是否处于"息屏+暂停" */
static int64_t s_pause_start_us = 0;    /* 本次"息屏+暂停"开始时刻（esp_timer，us） */

static void screen_sleep_check_pause_shutdown(void)
{
#if PAUSE_SHUTDOWN_MIN > 0
    /* 亮屏：不计时（清标志，下次息屏重新起算） */
    if (!s_sleeping) {
        s_prev_paused = false;
        return;
    }
    const mp3_player_state_t st = mp3_player_get_state();
    if (st == MP3_STATE_PAUSED) {
        if (!s_prev_paused) {
            /* 进入"息屏+暂停"：记录开始时刻 */
            s_prev_paused = true;
            s_pause_start_us = esp_timer_get_time();
        } else if (esp_timer_get_time() - s_pause_start_us
                   >= (int64_t)PAUSE_SHUTDOWN_MIN * 60 * 1000000LL) {
            ESP_LOGI(TAG, "paused (screen off) for %d min, auto fake shutdown",
                     PAUSE_SHUTDOWN_MIN);
            s_prev_paused = false;
            fake_shutdown_enter();   /* 进入深度休眠，不返回 */
        }
    } else {
        s_prev_paused = false;   /* 播放中/停止：暂停计时清零 */
    }
#endif
}

/* 无操作超时检测任务：亮屏且距最近活动超过配置时长 → 自动息屏；
   同时检查暂停超时自动伪关机（省电，无论屏幕亮暗） */
static void screen_sleep_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCREEN_SLEEP_CHECK_MS));

        /* 1. 息屏超时：仅亮屏时检查（息屏期间只允许按键唤醒） */
        if (!s_sleeping) {
            uint32_t timeout_s = config_store_get_screen_off_s();
            if (timeout_s != 0
                    && esp_timer_get_time() - s_last_activity_us
                       >= (int64_t)timeout_s * 1000000LL) {
                ESP_LOGI(TAG, "no activity for %u s, auto screen off",
                         (unsigned)timeout_s);
                screen_sleep_enter();
            }
        }

        /* 2. 暂停超时自动伪关机：与息屏状态无关，暂停过久即关机省电 */
        screen_sleep_check_pause_shutdown();
    }
}

void screen_sleep_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }

    /* 1. 初始固定 240MHz（亮屏状态）。
          ⚠ esp_pm_configure 依赖 CONFIG_PM_ENABLE=y（见 sdkconfig.defaults）；
          未开启时返回 ESP_ERR_NOT_SUPPORTED，此处降级：不调频，其余功能正常。 */
    if (screen_sleep_set_cpu_freq(PM_CPU_FREQ_ON_MHZ) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed (CONFIG_PM_ENABLE off?), freq fixed at boot value");
    } else {
        ESP_LOGI(TAG, "esp_pm ready: screen on=%d MHz, screen off=%d MHz (fixed)",
                 PM_CPU_FREQ_ON_MHZ, PM_CPU_FREQ_OFF_MHZ);
    }

    /* 2. 状态互斥锁 */
    s_mux = xSemaphoreCreateMutex();
    assert(s_mux);

    /* 3. 活动计时基准：从初始化时刻起算（开机后无操作超时从此刻开始） */
    s_last_activity_us = esp_timer_get_time();

    /* 4. 订阅触摸活动：按下算一次活动（复用 LVGL indev 轮询，无额外轮询） */
    lvgl_port_register_touch_cb(screen_sleep_touch_cb, NULL);

    /* 5. 无操作超时检测任务 */
    if (xTaskCreatePinnedToCore(screen_sleep_task, "screen_sleep",
                                SCREEN_SLEEP_TASK_STACK, NULL,
                                SCREEN_SLEEP_TASK_PRIO, NULL,
                                SCREEN_SLEEP_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create screen sleep task failed");
    }

    inited = true;
    ESP_LOGI(TAG, "screen sleep ready (auto off after %u s)",
             (unsigned)config_store_get_screen_off_s());
}

/* ============ 息屏/亮屏"重活"（由 deep_worker 大栈任务执行） ============
   原 screen_sleep_enter/exit 的执行体：esp_pm_configure（跨核 CCOMPARE
   同步，历史 2KB 栈实测爆栈）、AMOLED 休眠/唤醒（display_sleep_exit 含
   120ms 等待 + esp_lcd_panel_init 重初始化深链）、lv_refr_now 全屏同步重刷、
   lvgl_port_suspend/resume 等，调用链很深。集中到 deep_worker 任务（8KB 栈）
   执行后，任何调用方（超时检测任务/按键任务）只需浅投递 + 等待完成；
   deep_worker_call 失败（未初始化/卡死超时）时由调用方就地兜底执行，
   保证屏幕与 LVGL 状态始终一致。 */

/* 息屏重活：AMOLED 休眠 → 挂起 LVGL → 固定 80MHz */
static void screen_sleep_do_enter(void *arg)
{
    (void)arg;
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

    /* 3. 固定 CPU 频率 80MHz（省电）。⚠ 若 MP3 解码卡顿，可改 PM_CPU_FREQ_OFF_MHZ=160 */
    if (screen_sleep_set_cpu_freq(PM_CPU_FREQ_OFF_MHZ) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(80MHz) failed, freq unchanged");
    }

    ESP_LOGI(TAG, "screen off: AMOLED sleep in, LVGL suspended, CPU fixed %d MHz",
             PM_CPU_FREQ_OFF_MHZ);
}

/* 亮屏重活：240MHz → 持锁唤醒 AMOLED + 全屏重刷 + 恢复亮度 → resume LVGL */
static void screen_sleep_do_exit(void *arg)
{
    (void)arg;
    /* 1. 恢复 CPU 频率 240MHz（先升频再恢复显示，避免显示恢复瞬间系统还在低频） */
    if (screen_sleep_set_cpu_freq(PM_CPU_FREQ_ON_MHZ) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(240MHz) failed, freq unchanged");
    }

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

void screen_sleep_enter(void)
{
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_sleeping) {
        xSemaphoreGive(s_mux);
        return;
    }
    s_sleeping = true;

    /* 状态机翻转后，把深链投递给 deep_worker 大栈任务执行并等待完成
       （本调用方只做浅投递，不扛深调用链）；失败（executor 未初始化/
       卡死超时）时就地兜底执行，保证屏幕与 LVGL 状态一致。
       ⚠ s_mux 全程持有：息屏动作同步完成后才返回，快速连按由 s_mux
       串行化（见 SCREEN_SLEEP_AMOLED_OPTIMIZATION.md 第 5 节时序）。 */
    if (!deep_worker_call(screen_sleep_do_enter, NULL)) {
        screen_sleep_do_enter(NULL);
    }
    xSemaphoreGive(s_mux);
}

void screen_sleep_exit(void)
{
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_sleeping) {
        xSemaphoreGive(s_mux);
        return;
    }
    s_sleeping = false;

    /* 同 enter：深链交给 deep_worker 执行（含全屏重刷），失败就地兜底 */
    if (!deep_worker_call(screen_sleep_do_exit, NULL)) {
        screen_sleep_do_exit(NULL);
    }

    /* 重置活动计时：刚亮屏，避免立刻再次自动息屏 */
    s_last_activity_us = esp_timer_get_time();

    xSemaphoreGive(s_mux);
}

void screen_sleep_toggle(void)
{
    if (s_sleeping) {
        screen_sleep_exit();
    } else {
        screen_sleep_enter();
    }
}

bool screen_sleep_is_sleeping(void)
{
    return s_sleeping;
}

void screen_sleep_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

uint32_t screen_sleep_get_idle_sec(void)
{
    int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
    if (idle_us < 0) {
        idle_us = 0;
    }
    return (uint32_t)(idle_us / 1000000);
}
