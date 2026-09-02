#include "core/utils/battery_logger.h"

#include <stdio.h>

#include "core/hardware/battery.h"
#include "core/hardware/sd_card.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "batt_log";

/* 采样间隔（分钟）：经 Kconfig CONFIG_BATTERY_LOGGER_MIN 配置（默认 5，
   0=关闭）。测试/功耗测量用，测试完建议关闭。 */
#ifdef CONFIG_BATTERY_LOGGER_MIN
#define BATT_LOG_INTERVAL_MIN  CONFIG_BATTERY_LOGGER_MIN
#else
#define BATT_LOG_INTERVAL_MIN  0
#endif

/* 每个采样窗口的读数次数与间隔（ms）：10 秒窗口内读 10 次（1s 一次），
   去掉一个最高/最低值后对剩余 8 个求平均——消除瞬时噪声与 ADC 抖动，
   比单次读数稳定（10 秒窗口跨过播放负载的瞬时波动）。 */
#define BATT_LOG_SAMPLE_NUM   10
#define BATT_LOG_SAMPLE_GAP_MS 1000

/* 日志文件路径（SD 卡根目录，CSV 文本，追加写） */
#define BATT_LOG_FILE_PATH    "/sdcard/battery_log.txt"

/* 会话分隔行：每次开机（新会话）在文件里追加一行标记，把不同轮测试的
   数据段隔开。以 # 开头（CSV 注释行）；不带序号/时间——会话先后顺序
   即文件内出现顺序，按标记行切分即可区分各轮。 */
#define BATT_LOG_SESSION_MARKER  "# ===== session start ====="

/* 任务参数：Core1 最低优先级（几乎不占 CPU，每 5 分钟才忙 10 秒）。
   ⚠ 栈 4KB：FATFS 经 VFS 的 fopen/fprintf/fclose 调用链较深，3KB 偏紧，
   参考 cover_load(6K 解码)/sleep_timer(4K 因 I2C 深链) 的实测，取 4K 稳妥。 */
#define BATT_LOG_TASK_STACK   (4 * 1024)
#define BATT_LOG_TASK_PRIO    1
#define BATT_LOG_TASK_CORE    1

/* 写会话分隔行：开机即执行一次（不等首个采样周期——即使本次开机不足一个
   采样间隔，文件里也留下会话痕迹）。文件不存在时先写表头再写分隔行。 */
static void battery_logger_write_session_marker(void)
{
    FILE *f = fopen(BATT_LOG_FILE_PATH, "a");
    if (f == NULL) {
        return;
    }
    if (ftell(f) == 0) {
        /* 空文件（首次写入）：先写表头便于识别与后续分析 */
        fprintf(f, "elapsed_s,run_time_hms,avg_mv\n");
    }
    fprintf(f, BATT_LOG_SESSION_MARKER "\n");
    fclose(f);
}

/* 采样日志任务：每间隔唤醒一次 →
   1. 10 秒窗口采样 N 次电压（间隔 1s），失败读数丢弃；
   2. 去掉一个最高值 + 一个最低值，剩余求平均；
   3. "时间戳 + 平均电压"追加写 SD 文件（首次先写表头）。 */
static void battery_logger_task(void *arg)
{
    (void)arg;
    const uint32_t interval_ms = (uint32_t)BATT_LOG_INTERVAL_MIN * 60 * 1000;
    bool sd_missing_warned = false;

    /* 0. 会话开始标记：开机立即写一行分隔行（SD 未挂载时 fopen 失败直接
           返回，无副作用；文件不存在时顺带写表头） */
    battery_logger_write_session_marker();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        /* SD 卡未挂载（无卡/挂载失败）：跳过本轮写入。
           ⚠ 采样也不执行——记录必须落盘，空采无意义；首次缺失打一次日志 */
        if (!sd_card_is_mounted()) {
            if (!sd_missing_warned) {
                ESP_LOGW(TAG, "SD card not mounted, skip battery logging "
                              "(will retry every %d min)",
                         BATT_LOG_INTERVAL_MIN);
                sd_missing_warned = true;
            }
            continue;
        }
        sd_missing_warned = false;

        /* 1. 10 秒窗口采样：读取失败（battery_read_voltage_mv 返回 <0）的
              读数直接丢弃，不参与去极值与平均 */
        float samples[BATT_LOG_SAMPLE_NUM];
        int n = 0;
        for (int i = 0; i < BATT_LOG_SAMPLE_NUM; i++) {
            float v = battery_read_voltage_mv();
            if (v >= 0) {
                samples[n++] = v;
            }
            vTaskDelay(pdMS_TO_TICKS(BATT_LOG_SAMPLE_GAP_MS));
        }

        /* 2. 有效样本太少（<3 个不足以去极值）：本轮放弃，防写入垃圾行 */
        if (n < 3) {
            ESP_LOGW(TAG, "only %d valid samples, skip this log entry", n);
            continue;
        }

        /* 去掉一个最高值 + 一个最低值后求平均（线性扫描找极值即可） */
        float sum = 0.0f, vmax = samples[0], vmin = samples[0];
        for (int i = 0; i < n; i++) {
            sum += samples[i];
            if (samples[i] > vmax) {
                vmax = samples[i];
            }
            if (samples[i] < vmin) {
                vmin = samples[i];
            }
        }
        float avg_mv = (sum - vmax - vmin) / (float)(n - 2);

        /* 3. 时间戳：无 RTC，用开机以来运行时长（esp_timer）。
              每行 CSV：elapsed_s, HH:MM:SS, 平均电压(mV, 1 位小数) */
        int64_t elapsed_us = esp_timer_get_time();
        uint32_t elapsed_s = (uint32_t)(elapsed_us / 1000000LL);
        uint32_t hh = elapsed_s / 3600;
        uint32_t mm = (elapsed_s % 3600) / 60;
        uint32_t ss = elapsed_s % 60;

        FILE *f = fopen(BATT_LOG_FILE_PATH, "a");
        if (f == NULL) {
            ESP_LOGE(TAG, "open %s failed", BATT_LOG_FILE_PATH);
            continue;
        }
        if (ftell(f) == 0) {
            /* 空文件（首次写入）：写一行表头便于识别与后续分析 */
            fprintf(f, "elapsed_s,run_time_hms,avg_mv\n");
        }
        fprintf(f, "%u,%02u:%02u:%02u,%.1f\n",
                (unsigned)elapsed_s, (unsigned)hh, (unsigned)mm, (unsigned)ss,
                (double)avg_mv);
        fclose(f);

        ESP_LOGI(TAG, "log: %u s (%.1f V avg of %d samples) -> %s",
                 (unsigned)elapsed_s, (double)(avg_mv / 1000.0f), n,
                 BATT_LOG_FILE_PATH);
    }
}

void battery_logger_init(void)
{
#if BATT_LOG_INTERVAL_MIN > 0
    static bool inited = false;
    if (inited) {
        return;
    }
    if (xTaskCreatePinnedToCore(battery_logger_task, "batt_log",
                                BATT_LOG_TASK_STACK, NULL,
                                BATT_LOG_TASK_PRIO, NULL,
                                BATT_LOG_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create battery logger task failed");
        return;
    }
    inited = true;
    ESP_LOGI(TAG, "battery logger ready: every %d min, %d samples over %d s, "
                  "log file %s",
             BATT_LOG_INTERVAL_MIN, BATT_LOG_SAMPLE_NUM,
             BATT_LOG_SAMPLE_NUM * BATT_LOG_SAMPLE_GAP_MS / 1000,
             BATT_LOG_FILE_PATH);
#else
    ESP_LOGI(TAG, "battery logger disabled (CONFIG_BATTERY_LOGGER_MIN=0)");
#endif
}
