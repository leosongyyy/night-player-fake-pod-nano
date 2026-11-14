#include "core/hardware/audio/bt_audio.h"

#include <math.h>
#include <string.h>

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/eq.h"
#include "core/hardware/audio/resampler.h"
#include "core/utils/power_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bt_audio";

/* 输出缓冲帧容量：512 输入帧 × 最大 1.5 倍上采样（32k→48k）+ 相位余量 */
#define BT_AUDIO_OUT_FRAMES  1024

/* ringbuf 满时最长等待（ms）：给播放任务一点背压，但绝不能卡死（暂停/切歌
 * 需要它尽快返回）；超时说明蓝牙侧严重滞后，丢块比拖死整条链路好。
 * ⚠ 2026-11 真机蓝牙实测：debug 的 performance_monitor 每 2 秒采集一次任务
 * 统计（会短暂停住系统 ~20ms），20ms 的等待会导致 ringbuf 每 2 秒丢 512 帧
 * （约 10ms 音频，可闻的周期性"咔哒"）。放宽到 100ms 后即可吸收这种停顿；
 * release 没有性能监视，更不会触发。 */
#define BT_AUDIO_SEND_WAIT_MS  100

static RingbufHandle_t s_rb = NULL;
static SemaphoreHandle_t s_lock = NULL;   /* 保护激活状态与重采样器（写线程/控制线程） */

static resampler_t s_rs;
static volatile bool s_active = false;
static volatile bool s_inited = false;
static volatile bool s_streaming = false;
static uint32_t s_src_fs = 0;
static uint32_t s_out_fs = 0;

static uint32_t s_gain_q16 = AUDIO_REPLAY_GAIN_UNITY;
static float s_soft_gain = 1.0f;

/* DSP 临时缓冲（PSRAM）：int16 → float、重采样输出、最终 int16 */
static float   *s_in_f32 = NULL;
static float   *s_out_f32 = NULL;
static int16_t *s_i16 = NULL;

/* EQ 运行时（在 BT 线程内演化；参数版本变化时整体重载） */
static eq_runtime_t s_eq_rt;

/* 绝对音量 / 流控制回调（bt_service 注册） */
static bt_audio_absvol_cb_t s_absvol_cb = NULL;
static volatile bool s_absvol = false;
static bt_audio_stream_cb_t s_stream_cb = NULL;

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

esp_err_t bt_audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_rb = xRingbufferCreateWithCaps(BT_AUDIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rb == NULL) {
        ESP_LOGE(TAG, "create PCM ringbuf failed");
        return ESP_ERR_NO_MEM;
    }
    s_in_f32  = psram_alloc(BT_AUDIO_MAX_FRAMES * 2 * sizeof(float));
    s_out_f32 = psram_alloc(BT_AUDIO_OUT_FRAMES * 2 * sizeof(float));
    s_i16     = psram_alloc(BT_AUDIO_OUT_FRAMES * 2 * sizeof(int16_t));
    if (s_in_f32 == NULL || s_out_f32 == NULL || s_i16 == NULL) {
        ESP_LOGE(TAG, "allocate DSP buffers failed");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_eq_rt, 0, sizeof(s_eq_rt));
    s_inited = true;
    ESP_LOGI(TAG, "ready: ringbuf %u KB, out cap %d frames",
             (unsigned)(BT_AUDIO_RING_BYTES / 1024), BT_AUDIO_OUT_FRAMES);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 激活 / 采样率                                                        */
/* ------------------------------------------------------------------ */

void bt_audio_output_start(uint32_t src_fs, uint32_t out_fs)
{
    if (!s_inited || s_lock == NULL) {
        return;
    }
    if (src_fs == 0 || out_fs == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 换输出起点：清掉旧数据/滤波器历史，避免上一路径的残留 */
    bt_audio_flush();
    eq_reset_state();

    if (s_rs.inited && s_rs.fs_in == src_fs && s_rs.fs_out == out_fs) {
        resampler_reset(&s_rs);
    } else if (resampler_init(&s_rs, src_fs, out_fs, BT_AUDIO_MAX_FRAMES) != ESP_OK) {
        ESP_LOGE(TAG, "resampler init failed (%u -> %u)", (unsigned)src_fs,
                 (unsigned)out_fs);
        s_active = false;
        xSemaphoreGive(s_lock);
        return;
    }
    s_src_fs = src_fs;
    s_out_fs = out_fs;
    s_active = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "output active: %u -> %u Hz", (unsigned)src_fs,
             (unsigned)out_fs);
}

void bt_audio_output_stop(void)
{
    if (!s_inited || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_active;
    s_active = false;
    bt_audio_flush();
    if (s_rs.inited) {
        resampler_reset(&s_rs);
    }
    xSemaphoreGive(s_lock);

    if (was) {
        ESP_LOGI(TAG, "output stopped");
    }
}

bool bt_audio_is_active(void)
{
    return s_active;
}

void bt_audio_set_source_fs(uint32_t fs)
{
    if (!s_inited || fs == 0 || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active && fs != s_src_fs && s_out_fs != 0) {
        if (resampler_init(&s_rs, fs, s_out_fs, BT_AUDIO_MAX_FRAMES) == ESP_OK) {
            s_src_fs = fs;
            resampler_reset(&s_rs);
            ESP_LOGI(TAG, "source fs -> %u Hz", (unsigned)fs);
        } else {
            ESP_LOGE(TAG, "resampler re-init failed for %u Hz", (unsigned)fs);
        }
    } else {
        s_src_fs = fs;
    }
    xSemaphoreGive(s_lock);
}

void bt_audio_set_out_fs(uint32_t fs)
{
    if (!s_inited || fs == 0 || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (fs != s_out_fs) {
        uint32_t src = (s_src_fs != 0) ? s_src_fs : fs;
        if (resampler_init(&s_rs, src, fs, BT_AUDIO_MAX_FRAMES) == ESP_OK) {
            s_out_fs = fs;
            resampler_reset(&s_rs);
            ESP_LOGI(TAG, "output fs -> %u Hz", (unsigned)fs);
        } else {
            ESP_LOGE(TAG, "resampler re-init failed for %u Hz out", (unsigned)fs);
        }
    }
    xSemaphoreGive(s_lock);
}

uint32_t bt_audio_get_out_fs(void)
{
    return s_out_fs;
}

/* ------------------------------------------------------------------ */
/* PCM 写入与 DSP 链                                                     */
/* ------------------------------------------------------------------ */

void bt_audio_flush(void)
{
    if (s_rb == NULL) {
        return;
    }
    size_t sz = 0;
    void *item = NULL;
    while ((item = xRingbufferReceiveUpTo(s_rb, &sz, 0, BT_AUDIO_RING_BYTES)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
}

static inline int16_t f32_to_s16(float v)
{
    if (v > 1.0f) {
        v = 1.0f;
    } else if (v < -1.0f) {
        v = -1.0f;
    }
    return (int16_t)(v * 32767.0f);
}

void bt_audio_write(const int16_t *samples, size_t frames)
{
    if (!s_active || !s_inited || samples == NULL || frames == 0 || s_lock == NULL) {
        return;
    }
    if (!s_rs.inited || s_out_fs == 0) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_lock);
        return;
    }

    const float gain = ((float)s_gain_q16 * (1.0f / 65536.0f)) * s_soft_gain;
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > BT_AUDIO_MAX_FRAMES) {
            chunk = BT_AUDIO_MAX_FRAMES;
        }
        const int16_t *src = samples + done * 2;

        /* int16 → float（±1.0） */
        for (size_t i = 0; i < chunk * 2; i++) {
            s_in_f32[i] = (float)src[i] * (1.0f / 32768.0f);
        }

        /* 重采样到蓝牙输出采样率（EQ 之前） */
        size_t n = resampler_process(&s_rs, s_in_f32, chunk,
                                     s_out_f32, BT_AUDIO_OUT_FRAMES);
        if (n > 0) {
            /* EQ（系数已按 s_out_fs 计算）→ ×回放增益×软件音量 → int16 */
            eq_snapshot(&s_eq_rt);
            for (size_t i = 0; i < n; i++) {
                float xl = eq_process_sample(&s_eq_rt, 0, s_out_f32[2 * i]);
                float xr = eq_process_sample(&s_eq_rt, 1, s_out_f32[2 * i + 1]);
                s_i16[2 * i]     = f32_to_s16(xl * gain);
                s_i16[2 * i + 1] = f32_to_s16(xr * gain);
            }
            if (xRingbufferSend(s_rb, s_i16, n * 2 * sizeof(int16_t),
                                pdMS_TO_TICKS(BT_AUDIO_SEND_WAIT_MS)) != pdTRUE) {
                /* 蓝牙侧长时间不取数据（链路卡住）：丢弃本块，避免拖死播放 */
                static uint32_t s_drop_log_ms = 0;
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if (now_ms - s_drop_log_ms > 2000) {
                    s_drop_log_ms = now_ms;
                    ESP_LOGW(TAG, "ringbuf full, drop %u frames", (unsigned)n);
                }
            }
        }
        done += chunk;
    }
    xSemaphoreGive(s_lock);
}

void bt_audio_set_gain_q16(uint32_t gain_q16)
{
    if (gain_q16 == 0) {
        gain_q16 = AUDIO_REPLAY_GAIN_UNITY;
    }
    s_gain_q16 = gain_q16;
}

void bt_audio_set_soft_gain(float gain)
{
    if (gain < 0.0f) {
        gain = 0.0f;
    } else if (gain > 1.0f) {
        gain = 1.0f;
    }
    s_soft_gain = gain;
}

/* ------------------------------------------------------------------ */
/* 绝对音量 / 流控制                                                     */
/* ------------------------------------------------------------------ */

void bt_audio_set_absvol_handler(bt_audio_absvol_cb_t cb)
{
    s_absvol_cb = cb;
}

void bt_audio_set_absvol_supported(bool supported)
{
    s_absvol = supported;
}

bool bt_audio_absvol_supported(void)
{
    return s_absvol;
}

void bt_audio_send_absvol(uint8_t level)
{
    if (s_absvol && s_absvol_cb != NULL) {
        s_absvol_cb(level);
    }
}

void bt_audio_set_stream_handler(bt_audio_stream_cb_t cb)
{
    s_stream_cb = cb;
}

void bt_audio_request_stream(bool start)
{
    if (s_stream_cb != NULL && s_active) {
        s_stream_cb(start);
    }
}

void bt_audio_notify_streaming(bool streaming)
{
    if (s_streaming == streaming) {
        return;
    }
    s_streaming = streaming;
    /* 播放线程的性能需求：蓝牙音频（重采样 + SBC 编码）在熄屏时不能只跑
       80MHz（实测会卡顿），交给 power_service 提高熄屏频率 */
    power_service_perf_set(POWER_PERF_BT_AUDIO, streaming);
    ESP_LOGI(TAG, "streaming %s", streaming ? "started" : "stopped");
}

/* ------------------------------------------------------------------ */
/* A2DP 数据回调（Bluedroid A2DP source 任务上下文）                     */
/* ------------------------------------------------------------------ */

/**
 * @brief 协议栈拉取 PCM（内部 SBC 编码前）
 *
 * 约定：必须填满 len 字节；数据不足时补 0（静音），返回 len。
 * 这里只做 ringbuf 拷贝，不做任何 DSP/阻塞操作。
 */
int32_t bt_audio_data_cb(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        return 0;
    }
    /* 统计协议栈的拉取速率/欠载量：用于核对"实际 PCM 采样率"与播放速度
       （正常 48k 立体声 16bit = 192000 B/s；44.1k = 176400 B/s） */
    static uint32_t s_cb_bytes = 0;
    static uint32_t s_cb_miss = 0;
    static int64_t  s_cb_t0 = 0;

    size_t want = (size_t)len;
    size_t got = 0;
    while (got < want) {
        size_t sz = 0;
        void *item = xRingbufferReceiveUpTo(s_rb, &sz, 0, want - got);
        if (item == NULL) {
            break;
        }
        memcpy(buf + got, item, sz);
        vRingbufferReturnItem(s_rb, item);
        got += sz;
    }
    if (got < want) {
        memset(buf + got, 0, want - got);
    }

    s_cb_bytes += want;
    s_cb_miss += (uint32_t)(want - got);
    int64_t now = esp_timer_get_time();
    if (s_cb_t0 == 0) {
        s_cb_t0 = now;
    }
    if (now - s_cb_t0 >= 2000000) {
        double secs = (double)(now - s_cb_t0) / 1000000.0;
        ESP_LOGI(TAG, "stack pull: %.1f kB/s (%.1f kframes/s), miss %u B (%.3f%%), last_len=%d",
                 (double)s_cb_bytes / secs / 1000.0,
                 (double)s_cb_bytes / 4.0 / secs / 1000.0,
                 (unsigned)s_cb_miss,
                 100.0 * (double)s_cb_miss / (double)(s_cb_bytes ? s_cb_bytes : 1),
                 (int)len);
        s_cb_bytes = 0;
        s_cb_miss = 0;
        s_cb_t0 = now;
    }
    return len;
}
