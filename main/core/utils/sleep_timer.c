#include "core/utils/sleep_timer.h"

#include "core/hardware/audio/audio.h"
#include "core/storage/config_store.h"
#include "core/utils/fake_shutdown.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sleep_timer";

/* 任务参数：Core1 低优先级后台任务。到点触发伪关机（内部会等待播放停止
   并关外设），放独立任务里不影响 LVGL/UI 线程。500ms 周期兼顾到期精度
   与音量渐降步进（30s 内约 60 次更新，每步约降 1 档）。
   ⚠ 栈需 4096：渐降要直接调 audio_set_volume_level() 走旧版 I2C 驱动写
   CS43131（互斥锁 + 中断路径栈开销大），2048B 曾栈溢出（vApplication
   StackOverflowHook panic，见 xQueueTakeMutexRecursive 回溯帧）。 */
#define SLEEP_CHECK_MS   500
#define SLEEP_TASK_STACK 4096
#define SLEEP_TASK_PRIO  4
#define SLEEP_TASK_CORE  1

/* 音量渐降窗口（ms）：剩余时间 <= 该值时从当前音量线性降到 0 档
   （NightPlayer 原版机制；只写 audio 硬件、不写 config_store，不持久化） */
#define VOL_FADE_MS      30000

/* 清醒感知（NightPlayer 原版机制）：
   - 睡眠定时开启期间，用户动作（切歌/播放控制/点歌等，见
     sleep_timer_note_activity 的调用点）触发"加时"；
   - ⚠ 音量调节刻意不参与：本模块只把切歌/播放控制等"听觉上仍在活动"
     的动作视为用户清醒信号（NightPlayer 原版含调音量，此处已剔除）；
   - 追加量 = (总时长 - 当前剩余时间) × 0.382（0.382 = 1 - 0.618，黄金分割
     互补系数）：即每次把剩余时间向总时长"拉回"38.2%——剩余越少追加越多
     （快到时切歌回血明显），刚设置时追加很少；无需记录定时开始时刻，
     完全由"当前剩余"决定，连续触发逐次趋近总时长；
   - ⚠ 原版无冷却：连续按键说明用户必然清醒，每次动作都立即折算加时
     （原版实现如此，经长期使用验证）。 */
#define AWARE_ADD_NUM    382        /* 追加系数分子（0.382 = 382/1000） */
#define AWARE_ADD_DEN    1000       /* 追加系数分母 */

/* 当前定时状态：s_minutes=0 表示关闭；s_deadline_us 为绝对到期时刻。
   仅设置页（LVGL 任务）写入、本任务读取，volatile 保证可见性。 */
static volatile uint32_t s_minutes = 0;
static volatile int64_t  s_deadline_us = 0;

/* 音量渐降状态：s_fade_active/s_fade_origin 仅由本任务修改（恢复也由任务
   统一执行），外部只置 s_restore_requested 请求，避免跨任务直接写音量
   产生竞态。s_last_fade_level 仅本任务使用。 */
static volatile bool    s_fade_active = false;       /* 是否正在渐降 */
static volatile uint8_t s_fade_origin = 0;           /* 渐降起点音量（恢复用） */
static uint8_t          s_last_fade_level = 0;       /* 最近一次写入的渐降档位 */
static volatile bool    s_restore_requested = false; /* 取消/延长定时后请求恢复 */

/* 倒计时任务：到点调用伪关机（不返回）；最后 30 秒执行音量渐降 */
static void sleep_timer_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SLEEP_CHECK_MS));

        /* 恢复请求：取消/延长定时后由 sleep_timer_set 置位，恢复为渐降前
           音量（渐降值从不落 NVS，下次开机仍为用户原音量） */
        if (s_restore_requested) {
            s_restore_requested = false;
            if (s_fade_active) {
                s_fade_active = false;
                audio_set_volume_level(s_fade_origin);
                ESP_LOGI(TAG, "volume fade stopped, restored to level %u",
                         (unsigned)s_fade_origin);
            }
        }

        if (s_minutes == 0) {
            continue;
        }

        int64_t remain_us = s_deadline_us - esp_timer_get_time();
        if (remain_us <= 0) {
            ESP_LOGI(TAG, "sleep timer expired (%u min), enter fake shutdown",
                     (unsigned)s_minutes);
            s_minutes = 0;
            fake_shutdown_enter();   /* 只创建关机任务并返回，不阻塞本任务 */

            /* ⚠ 必须跳过下面的渐降段：到点后 remain_us <= 0，若继续用
               remain_ms 计算渐降档位，负数强转 (uint32_t) 变成巨大值，
               再经 uint8_t 截断会写出任意音量档（曾出现关机瞬间音量
               跳变/爆音，听感比用户原音量还大）。 */
            continue;
        }

        /* 音量渐降：剩余 <= 30s 时启动，从当前音量线性降到 0 档。
           只写 audio 层（不改 config_store），渐降值不持久化 */
        if (!s_fade_active) {
            if (remain_us <= (int64_t)VOL_FADE_MS * 1000) {
                uint8_t cur = audio_get_volume_level();
                if (cur > 0) {
                    s_fade_active = true;
                    s_fade_origin = cur;
                    s_last_fade_level = cur;
                    ESP_LOGI(TAG, "volume fade start: level %u -> 0 (last %u s)",
                             (unsigned)cur, (unsigned)(VOL_FADE_MS / 1000));
                }
            }
        }
        if (s_fade_active) {
            /* 目标档位 = 起点档位 × 剩余时间 / 渐降窗口（线性，向下取整）。
               remain_us > 0（已处理到点），remain_ms 范围 0~30000，
               origin <= 30，乘积 < 2^32 无溢出 */
            int64_t remain_ms = remain_us / 1000;
            uint8_t target;
            if (remain_ms >= (int64_t)VOL_FADE_MS) {
                target = s_fade_origin;
            } else {
                target = (uint8_t)((uint32_t)s_fade_origin
                                   * (uint32_t)remain_ms / VOL_FADE_MS);
            }
            if (target != s_last_fade_level) {
                s_last_fade_level = target;
                audio_set_volume_level(target);
            }
        }
    }
}

void sleep_timer_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    BaseType_t ret = xTaskCreatePinnedToCore(sleep_timer_task, "sleep_timer",
                                             SLEEP_TASK_STACK, NULL,
                                             SLEEP_TASK_PRIO, NULL,
                                             SLEEP_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create sleep timer task failed");
        return;
    }
    inited = true;
    ESP_LOGI(TAG, "sleep timer ready (off by default)");
}

/* 开机默认应自动开启的分钟数（纯查询，无副作用）：config_store 的
   sleep_def_on + sleep_default_min 组合判定，main.c（真正启动定时）与
   boot_flow（弹 Toast 提醒）共用，保证两处判定一致。 */
uint32_t sleep_timer_boot_autostart_min(void)
{
    if (!config_store_get_sleep_def_on()) {
        return 0;                        /* 未开启"开机默认开启" */
    }
    uint32_t minutes = config_store_get_sleep_default_min();
    if (minutes == 0) {
        return 0;                        /* 默认时长为 0：不自动开启 */
    }
    return minutes;
}

void sleep_timer_set(uint32_t minutes)
{
    if (minutes == 0) {
        s_minutes = 0;
        ESP_LOGI(TAG, "sleep timer cancelled");
    } else {
        s_minutes = minutes;
        s_deadline_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000LL;
        ESP_LOGI(TAG, "sleep timer set: %u min", (unsigned)minutes);
    }
    /* 取消或重新设置（延长/缩短）定时：若正在音量渐降，置恢复请求，
       由倒计时任务统一恢复到渐降前音量（避免跨任务直接写音量） */
    if (s_fade_active) {
        s_restore_requested = true;
    }
}

uint32_t sleep_timer_get_minutes(void)
{
    return s_minutes;
}

uint32_t sleep_timer_get_remaining_s(void)
{
    if (s_minutes == 0) {
        return 0;
    }
    int64_t remain = s_deadline_us - esp_timer_get_time();
    if (remain <= 0) {
        return 0;   /* 已到点（任务即将触发关机），UI 显示 0 即可 */
    }
    /* 向上取整到秒，避免显示 14:00 时实际只剩 13:59.9 的误导 */
    return (uint32_t)((remain + 999999) / 1000000);
}

bool sleep_timer_is_active(void)
{
    return s_minutes > 0;
}

bool sleep_timer_fade_is_active(void)
{
    return s_fade_active;
}

void sleep_timer_fade_cancel(void)
{
    if (s_fade_active) {
        s_fade_active = false;
        ESP_LOGI(TAG, "volume fade cancelled (user override)");
    }
}

/* 清醒感知：用户动作（切歌/播放控制/点歌等）触发睡眠定时加时。
   NightPlayer 原版算法：追加量 = (总时长 - 当前剩余) × 0.382，即把剩余
   时间向总时长拉回 38.2%。示例：15 分钟定时当前剩余 12:30（已耗 2:30）时
   切歌 → 追加 = (15min - 12.5min)×0.382 ≈ 0.955min ≈ 57s，剩余变 13:27。
   原版无冷却——连续按键说明用户必然清醒，每次动作都立即追加。
   ⚠ 音量调节（按键/通知栏滑块）不再加时：调节音量不代表用户仍清醒，
     只在 UI 层真正的"切歌/播放控制"操作点调用（息屏按键播放控制、播放页
     控制、列表选歌等）；本函数只改到期时刻，不落 NVS（睡眠定时本身不持久化）。 */
void sleep_timer_note_activity(void)
{
    if (s_minutes == 0) {
        return;                              /* 未开启定时 */
    }
    if (!config_store_get_sleep_awake()) {
        return;                              /* 清醒感知关闭 */
    }
    int64_t now = esp_timer_get_time();

    /* 触发前的剩余时间（旧的到期时刻 - 现在），用于计算追加量与日志对比 */
    int64_t old_remain_us = s_deadline_us - now;
    if (old_remain_us < 0) {
        old_remain_us = 0;                   /* 兜底：不为负 */
    }

    /* 追加量 = (总时长 - 当前剩余) × 0.382（整数运算，避免浮点）。
       剩余越少追加越多（趋近总时长）；连续触发逐次拉回，永不超总时长 */
    int64_t total_us = (int64_t)s_minutes * 60 * 1000000LL;
    int64_t add_us   = (total_us - old_remain_us)
                       * AWARE_ADD_NUM / AWARE_ADD_DEN;
    if (add_us < 0) {
        add_us = 0;
    }
    int64_t remain_us = old_remain_us + add_us;

    s_deadline_us = now + remain_us;
    /* 保留 1 位小数：整数秒取整会把 <1s 的增量显示成 0，误导排查。
       例：remaining 750.0 s -> 807.3 s (adds 57.3 s, total 15 min) */
    ESP_LOGI(TAG, "sleep aware: remaining %.1f s -> %.1f s (adds %.1f s, "
                  "total %u min)",
             (double)(old_remain_us / 100000) / 10.0,
             (double)(remain_us / 100000) / 10.0,
             (double)(add_us / 100000) / 10.0,
             (unsigned)s_minutes);

    /* 加时后剩余回到渐降窗口之外：若正在渐降，请求任务恢复渐降前音量 */
    if (remain_us > (int64_t)VOL_FADE_MS * 1000 && s_fade_active) {
        s_restore_requested = true;
        ESP_LOGI(TAG, "sleep aware: remain > fade window, restore volume");
    }
}
