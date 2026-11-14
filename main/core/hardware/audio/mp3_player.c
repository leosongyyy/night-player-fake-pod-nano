#include "core/hardware/audio/mp3_player.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/audio_decoders/audio_decoder.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/eq.h"
#include "core/utils/task_psram.h"

#define PLAYER_PATH_MAX     512
/* 每次解码 512 帧（≈11.6ms @44.1k），与 audio.c 的输出块一致：
   小块高频写 I2S，缩短单次“解码→写”间隙，避免 80MHz 下 DMA 欠载。 */
#define PLAYER_PCM_FRAMES   512
/* Tremor 在首次解码时用 alloca 建 codebook 索引（约 8 字节/条目），
   24KB 对高复杂度 OGG 偏紧；栈在 PSRAM，直接给足余量。 */
#define PLAYER_TASK_STACK   (64 * 1024)
/* 9：高于 LVGL(2)/deep_worker(6)/player_ctrl(5)，低于 lwIP/timer 等系统任务。
   播放任务在 SD/I2S 上阻塞让出 CPU，不会忙等。 */
#define PLAYER_TASK_PRIO    9
#define PLAYER_TASK_CORE    1

static const char *TAG = "mp3_player";

static volatile mp3_player_state_t s_state = MP3_STATE_IDLE;
static volatile bool s_paused;
static volatile bool s_stop;
static volatile bool s_seek_req;
static volatile uint32_t s_seek_target;
static volatile uint32_t s_frames_played;
static volatile uint32_t s_sample_rate;
static volatile uint32_t s_duration_s;
static char s_path[PLAYER_PATH_MAX];
static uint32_t s_pending_gain = AUDIO_REPLAY_GAIN_UNITY;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
/* 串行化 mp3_player_play：player_controller 正常只在一个任务里调用，
 * 但停止回调里续播 + 用户/蓝牙事件可能并发进来，必须在入口互斥。 */
static SemaphoreHandle_t s_play_mux = NULL;
static mp3_player_stopped_cb_t s_stopped_cb;

static esp_err_t init_sync(void)
{
    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
    }
    if (s_play_mux == NULL) {
        s_play_mux = xSemaphoreCreateMutex();
    }
    return (s_done != NULL && s_play_mux != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void player_task(void *arg)
{
    (void)arg;
    mp3_player_stop_reason_t reason = MP3_STOP_NATURAL;
    audio_decoder_t *dec = NULL;
    int16_t *pcm = NULL;
    audio_fs_t fs = AUDIO_FS_48000;
    uint32_t channels = 2;
    uint32_t sr = 0;
    bool paused_applied = false;
    bool started = false;
    char err[128] = "";

    /* 区分"文件不存在"和"解码失败"，供控制器的坏曲分类使用 */
    FILE *probe = fopen(s_path, "rb");
    if (!probe) {
        reason = MP3_STOP_FILE_MISSING;
        goto cleanup;
    }
    fclose(probe);

    dec = audio_decoder_open(s_path, err, sizeof(err));
    if (!dec) {
        ESP_LOGE(TAG, "open decoder failed: %s (%s)", s_path, err);
        reason = MP3_STOP_DATA_CORRUPT;
        goto cleanup;
    }

    sr = audio_decoder_sample_rate(dec);
    channels = audio_decoder_channels(dec);
    if (sr != 32000 && sr != 44100 && sr != 48000) {
        ESP_LOGW(TAG, "unsupported sample rate: %u", (unsigned)sr);
        reason = MP3_STOP_FORMAT_UNSUPPORTED;
        goto cleanup;
    }
    if (channels != 1 && channels != 2) {
        ESP_LOGW(TAG, "unsupported channels: %u", (unsigned)channels);
        reason = MP3_STOP_FORMAT_UNSUPPORTED;
        goto cleanup;
    }
    fs = (audio_fs_t)sr;

    s_sample_rate = sr;
    s_frames_played = 0;
    s_duration_s = (uint32_t)(audio_decoder_total_frames(dec) / sr);

    if (s_stop) {
        reason = MP3_STOP_USER;
        goto cleanup;
    }

    /* PCM 只有 512×2×2 = 2KB，优先内部 RAM，避开 PSRAM 读延迟 */
    pcm = heap_caps_malloc(sizeof(int16_t) * PLAYER_PCM_FRAMES * 2,
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm) {
        pcm = heap_caps_malloc(sizeof(int16_t) * PLAYER_PCM_FRAMES * 2,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!pcm) {
        pcm = malloc(sizeof(int16_t) * PLAYER_PCM_FRAMES * 2);
    }
    if (!pcm) {
        reason = MP3_STOP_DATA_CORRUPT;
        goto cleanup;
    }

    /* 与旧曲的 EQ 历史状态解耦；采样率变化时 audio_configure_fs 会重算系数 */
    eq_reset_state();
    audio_set_replay_gain(s_pending_gain);

    if (audio_pcm_start(fs) != ESP_OK) {
        reason = MP3_STOP_DATA_CORRUPT;
        goto cleanup;
    }

    while (!s_stop) {
        if (s_seek_req) {
            uint32_t target = s_seek_target;
            if (s_duration_s && target > s_duration_s) {
                target = s_duration_s;
            }
            bool was_playing = !s_paused && !paused_applied;
            if (was_playing) {
                audio_pcm_stop();
            }
            if (audio_decoder_seek(dec, target) == ESP_OK) {
                s_frames_played = target * sr;
            } else {
                ESP_LOGW(TAG, "seek to %u s failed", (unsigned)target);
            }
            s_seek_req = false;
            if (was_playing) {
                if (audio_pcm_start(fs) != ESP_OK) {
                    reason = MP3_STOP_DATA_CORRUPT;
                    break;
                }
            }
        }

        if (s_paused) {
            if (!paused_applied) {
                audio_pcm_stop();
                paused_applied = true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (paused_applied) {
            if (audio_pcm_start(fs) != ESP_OK) {
                reason = MP3_STOP_DATA_CORRUPT;
                break;
            }
            paused_applied = false;
        }

        int frames = audio_decoder_read(dec, pcm, PLAYER_PCM_FRAMES);
        if (frames <= 0) {
            break;
        }
        started = true;

        if (channels == 1) {
            for (int i = frames - 1; i >= 0; i--) {
                pcm[i * 2] = pcm[i];
                pcm[i * 2 + 1] = pcm[i];
            }
        }

        if (audio_pcm_write(pcm, (size_t)frames) != ESP_OK) {
            reason = MP3_STOP_DATA_CORRUPT;
            break;
        }
        s_frames_played += (uint32_t)frames;
    }

    if (s_stop) {
        reason = MP3_STOP_USER;
    } else if (!started && reason == MP3_STOP_NATURAL) {
        reason = MP3_STOP_DATA_CORRUPT;
    }

cleanup:
    if (pcm) {
        free(pcm);
    }
    audio_pcm_stop();
    if (dec) {
        audio_decoder_close(dec);
    }

    s_stop = false;
    s_paused = false;
    s_seek_req = false;
    s_state = MP3_STATE_IDLE;
    s_task = NULL;
    if (s_done) {
        xSemaphoreGive(s_done);
    }

    /* 先置 IDLE / 清任务句柄，再回调：控制器可在回调中安全地发起下一首 */
    mp3_player_stopped_cb_t cb = s_stopped_cb;
    if (cb) {
        cb(reason);
    }
    ESP_LOGI(TAG, "player task exit (reason=%d)", (int)reason);
    vTaskDelete(NULL);
}

static esp_err_t mp3_play_locked(const char *path)
{
    if (!path || !path[0] || strlen(path) >= sizeof(s_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (init_sync() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /* 旧曲必须完全退出后再换 s_path，避免旧任务读到新路径 */
    if (s_task) {
        s_stop = true;
        s_paused = false;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "stop old track timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreTake(s_done, 0);

    snprintf(s_path, sizeof(s_path), "%s", path);
    s_paused = false;
    s_stop = false;
    s_seek_req = false;
    s_seek_target = 0;
    s_frames_played = 0;
    s_sample_rate = 0;
    s_duration_s = 0;
    s_state = MP3_STATE_PLAYING;

    BaseType_t created = task_create_psram_stack(player_task, "mp3_play",
                                                 PLAYER_TASK_STACK, NULL,
                                                 PLAYER_TASK_PRIO, &s_task,
                                                 PLAYER_TASK_CORE);
    if (created != pdPASS) {
        s_task = NULL;
        s_state = MP3_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mp3_player_play(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (init_sync() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_play_mux, portMAX_DELAY);
    esp_err_t ret = mp3_play_locked(path);
    xSemaphoreGive(s_play_mux);
    return ret;
}

esp_err_t mp3_player_pause(void)
{
    if (s_state != MP3_STATE_PLAYING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_paused = true;
    s_state = MP3_STATE_PAUSED;
    return ESP_OK;
}

esp_err_t mp3_player_resume(void)
{
    if (s_state != MP3_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }
    s_paused = false;
    s_state = MP3_STATE_PLAYING;
    return ESP_OK;
}

esp_err_t mp3_player_stop(void)
{
    if (s_state == MP3_STATE_IDLE && s_task == NULL) {
        return ESP_OK;
    }
    /* 非阻塞：与旧实现一致，由播放任务退出后置 IDLE */
    s_stop = true;
    s_paused = false;
    return ESP_OK;
}

esp_err_t mp3_player_seek(uint32_t target_s)
{
    if (s_state == MP3_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_seek_target = target_s;
    s_seek_req = true;
    return ESP_OK;
}

mp3_player_state_t mp3_player_get_state(void)
{
    return s_state;
}

uint32_t mp3_player_get_position_s(void)
{
    uint32_t sr = s_sample_rate;
    return sr ? s_frames_played / sr : 0;
}

uint32_t mp3_player_get_duration_s(void)
{
    return s_duration_s;
}

void mp3_player_set_stopped_callback(mp3_player_stopped_cb_t cb)
{
    s_stopped_cb = cb;
}

void mp3_player_set_gain(uint32_t gain_q16)
{
    s_pending_gain = gain_q16;
}