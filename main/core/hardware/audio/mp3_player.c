#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/audio/audio.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3common.h"
#include "mp3dec.h"

static const char *TAG = "mp3_player";

/* 解码输入缓冲（流式 fread，小块平滑读，够实时播放即可） */
#define MP3_READBUF_SIZE   (16 * 1024)

/* 输出缓冲：一帧解码的最大 PCM 样本数 = MAX_NCHAN(2) × MAX_NGRAN(2) × MAX_NSAMP(576) */
#define MP3_OUTBUF_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

/* 播放任务参数（Core1，避免与 LVGL/Core0 抢时间） */
#define MP3_PLAY_TASK_STACK 8192
#define MP3_PLAY_TASK_PRIO  8
#define MP3_PLAY_TASK_CORE  1

/* 缓冲放静态区（BSS），不占播放任务栈 */
static unsigned char read_buf[MP3_READBUF_SIZE];
static short out_buf[MP3_OUTBUF_SAMPLES];
static short stereo_buf[MP3_OUTBUF_SAMPLES];   /* 单声道 → 双声道转换缓冲 */

/* 播放路径（由 mp3_player_play 传入，须为静态字符串字面量） */
static const char *s_path = NULL;

/* 播放状态与控制 */
static volatile mp3_player_state_t s_state = MP3_STATE_IDLE;
static volatile bool s_pause_requested = false;
static volatile bool s_stop_requested = false;
static volatile bool s_seek_requested = false;
static volatile uint32_t s_seek_target_s = 0;
static SemaphoreHandle_t s_resume_sem = NULL;

/* 播放停止回调与停止来源标记 */
static mp3_player_stopped_cb_t s_stopped_cb = NULL;
static volatile bool s_stopped_by_user = false;   /* true=用户停止，false=自然播完 */

/* 进度跟踪（基于系统时钟，毫秒精度；32bit 原子读，免锁） */
static volatile uint32_t s_played_ms = 0;
static volatile uint32_t s_seg_start_ms = 0;
static volatile uint32_t s_duration_s = 0;

/* 当前采样率（暂停恢复 / seek 后重新配置 I2S 用） */
static audio_fs_t s_fs = AUDIO_FS_44100;

/* ------------------------------------------------------------------ */
/* ID3v2 标签跳过（FILE 版本）                                          */
/* ------------------------------------------------------------------ */

static void mp3_skip_id3v2(FILE *f)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fseek(f, 0, SEEK_SET);
        return;
    }
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint32_t size = ((uint32_t)hdr[6] << 21) | ((uint32_t)hdr[7] << 14) |
                        ((uint32_t)hdr[8] << 7) | (uint32_t)hdr[9];
        uint32_t skip = 10 + size;
        if (hdr[5] & 0x10) {
            skip += 10;
        }
        ESP_LOGI(TAG, "skip ID3v2 tag (%u bytes)", (unsigned)skip);
        fseek(f, (long)skip, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }
}

/* ------------------------------------------------------------------ */
/* 采样率映射                                                           */
/* ------------------------------------------------------------------ */

static uint32_t mp3_map_samprate(int samprate)
{
    switch (samprate) {
    case 32000: return 32000;
    case 44100: return 44100;
    case 48000: return 48000;
    default:    return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Xing/Info 头读总时长（无需扫描全文件，毫秒级）                        */
/* ------------------------------------------------------------------ */

/* MP3 事实标准：编码器在第一个 MPEG 帧的 side info 之后插入一个伪帧，
   记录总帧数 / 总字节数 / seek TOC。LAME 的 CBR 文件该魔数为 "Info"，
   VBR 文件为 "Xing"。读取它即可直接得到总时长。 */
static uint32_t mp3_read_xing_duration(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    /* 跳 ID3v2 */
    uint8_t hdr[10];
    uint32_t offset = 0;
    if (read(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
        hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint32_t size = ((uint32_t)hdr[6] << 21) | ((uint32_t)hdr[7] << 14) |
                        ((uint32_t)hdr[8] << 7) | (uint32_t)hdr[9];
        offset = 10 + size + ((hdr[5] & 0x10) ? 10 : 0);
    }
    lseek(fd, (off_t)offset, SEEK_SET);

    /* 第一个 MPEG 帧头 */
    uint8_t fh[4];
    if (read(fd, fh, 4) != 4) {
        close(fd);
        return 0;
    }

    int verIdx = (fh[1] >> 3) & 3;
    int ver = (verIdx == 0) ? 2 : ((verIdx & 1) ? 0 : 1);
    int layer = 4 - ((fh[1] >> 1) & 3);
    int brIdx = (fh[2] >> 4) & 15;
    int srIdx = (fh[2] >> 2) & 3;
    int chMode = (fh[3] >> 6) & 3;
    int crc = 1 - (fh[1] & 0x01);

    if (layer != 3 || brIdx == 0 || brIdx == 15 || srIdx == 3) {
        close(fd);
        return 0;
    }

    int samprate = samplerateTab[ver][srIdx];
    int spf = samplesPerFrameTab[ver][layer - 1];
    int bitrate_bps = bitrateTab[ver][layer - 1][brIdx] * 1000;
    int is_mono = (chMode == 3) ? 0 : 1;
    int side_info = sideBytesTab[ver][is_mono];

    /* Xing/Info 头位置 = 帧头(4) + CRC(2?) + side info */
    lseek(fd, (off_t)(offset + 4 + (crc ? 2 : 0) + side_info), SEEK_SET);

    uint8_t tag[8];
    if (read(fd, tag, 8) != 8) {
        close(fd);
        return 0;
    }

    if (!((tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g') ||
          (tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o'))) {
        close(fd);
        return 0;   /* 无 Xing/Info 头，调用方 CBR 估算兜底 */
    }

    uint32_t flags = ((uint32_t)tag[4] << 24) | ((uint32_t)tag[5] << 16) |
                     ((uint32_t)tag[6] << 8) | (uint32_t)tag[7];

    uint32_t frames = 0, bytes = 0;
    uint8_t b[4];
    if (flags & 0x0001) {
        if (read(fd, b, 4) == 4) frames = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                                          ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    }
    if (flags & 0x0002) {
        if (read(fd, b, 4) == 4) bytes = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                                         ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    }

    close(fd);

    uint32_t duration = 0;
    if (frames > 0 && samprate > 0) {
        duration = frames * (uint32_t)spf / (uint32_t)samprate;
    } else if (bytes > 0 && bitrate_bps > 0) {
        duration = bytes * 8 / (uint32_t)bitrate_bps;
    }

    ESP_LOGI(TAG, "xing: %u frames, %u bytes -> %u s",
             (unsigned)frames, (unsigned)bytes, (unsigned)duration);
    return duration;
}

/* 播放结束统一收尾：置 IDLE 并触发停止回调（此时 s_state 已是 IDLE，
   回调里可直接 mp3_player_play 续播下一曲） */
static void mp3_player_finish(void)
{
    s_state = MP3_STATE_IDLE;
    mp3_player_stopped_cb_t cb = s_stopped_cb;
    if (cb != NULL) {
        cb(s_stopped_by_user);
    }
}

/* ------------------------------------------------------------------ */
/* 播放任务                                                             */
/* ------------------------------------------------------------------ */

static void play_task(void *arg)
{
    (void)arg;

    /* 优先用 Xing/Info 头读总时长（毫秒级）；无则稍后 CBR 估算兜底 */
    s_duration_s = mp3_read_xing_duration(s_path);

    FILE *f = fopen(s_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed", s_path);
        mp3_player_finish();
        vTaskDelete(NULL);
        return;
    }

    mp3_skip_id3v2(f);
    uint32_t data_offset = (uint32_t)ftell(f);

    /* 音频数据长度（seek 字节映射与 CBR 兜底用） */
    fseek(f, 0, SEEK_END);
    uint32_t data_size = (uint32_t)ftell(f) - data_offset;
    fseek(f, (long)data_offset, SEEK_SET);

    HMP3Decoder dec = MP3InitDecoder();
    if (dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        fclose(f);
        mp3_player_finish();
        vTaskDelete(NULL);
        return;
    }

    int bytes_left = 0;
    unsigned char *read_ptr = read_buf;
    bool eof = false;
    bool started = false;

    while (true) {
        /* ---- 停止 ---- */
        if (s_stop_requested) {
            s_stop_requested = false;
            break;
        }

        /* ---- 暂停 ---- */
        if (s_pause_requested) {
            s_pause_requested = false;
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_played_ms += now_ms - s_seg_start_ms;
            audio_pcm_stop();
            s_state = MP3_STATE_PAUSED;
            ESP_LOGI(TAG, "paused @ %u ms", (unsigned)s_played_ms);
            xSemaphoreTake(s_resume_sem, portMAX_DELAY);
            if (s_stop_requested) {
                s_stop_requested = false;
                break;
            }
            s_seg_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            audio_pcm_start(s_fs);
            s_state = MP3_STATE_PLAYING;
            ESP_LOGI(TAG, "resumed");
        }

        /* ---- seek ---- */
        if (s_seek_requested) {
            s_seek_requested = false;
            uint32_t target = s_seek_target_s;
            if (s_duration_s > 0 && target > s_duration_s) {
                target = s_duration_s;
            }
            uint32_t byte_off = data_offset;
            if (s_duration_s > 0) {
                byte_off += (uint32_t)((uint64_t)data_size * target / s_duration_s);
            }
            fseek(f, (long)byte_off, SEEK_SET);
            bytes_left = 0;
            read_ptr = read_buf;
            eof = false;
            s_played_ms = target * 1000;
            s_seg_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            audio_pcm_start(s_fs);
            ESP_LOGI(TAG, "seek to %u s (byte %u)", (unsigned)target, (unsigned)byte_off);
        }

        /* ---- 补充数据 ---- */
        if (bytes_left < 2 * MAINBUF_SIZE && !eof) {
            memmove(read_buf, read_ptr, bytes_left);
            read_ptr = read_buf;
            int n = (int)fread(read_buf + bytes_left, 1, MP3_READBUF_SIZE - bytes_left, f);
            bytes_left += n;
            if (n == 0) {
                eof = true;
            }
            memset(read_buf + bytes_left, 0, MP3_READBUF_SIZE - bytes_left);
        }

        /* ---- 找同步字 ---- */
        int offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (offset < 0) {
            break;
        }
        read_ptr += offset;
        bytes_left -= offset;

        /* ---- 解码一帧（保存位置，错误时回退跳过假同步字） ---- */
        unsigned char *frame_ptr = read_ptr;
        int frame_bytes = bytes_left;
        int err = MP3Decode(dec, &read_ptr, &bytes_left, out_buf, 0);

        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);

            if (!started) {
                uint32_t fs = mp3_map_samprate(fi.samprate);
                if (fs == 0) {
                    ESP_LOGE(TAG, "unsupported sample rate %d Hz", fi.samprate);
                    break;
                }
                s_fs = (audio_fs_t)fs;

                /* 无 Xing/Info 头时，用首帧码率做 CBR 估算兜底 */
                if (s_duration_s == 0 && fi.bitrate > 0) {
                    s_duration_s = data_size * 8 / (uint32_t)fi.bitrate;
                    ESP_LOGI(TAG, "no xing, cbr estimate %u s", (unsigned)s_duration_s);
                }

                s_played_ms = 0;
                s_seg_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                esp_err_t r = audio_pcm_start(s_fs);
                if (r != ESP_OK) {
                    ESP_LOGE(TAG, "audio_pcm_start failed: %s", esp_err_to_name(r));
                    break;
                }
                s_state = MP3_STATE_PLAYING;
                started = true;
                ESP_LOGI(TAG, "mp3: %d Hz, %d ch, %d kbps, dur %u s",
                         fi.samprate, fi.nChans, fi.bitrate / 1000, (unsigned)s_duration_s);
            }

            esp_err_t wret;
            if (fi.nChans == 2) {
                wret = audio_pcm_write(out_buf, (size_t)(fi.outputSamps / 2));
            } else if (fi.nChans == 1) {
                for (int i = 0; i < fi.outputSamps; i++) {
                    stereo_buf[2 * i]     = out_buf[i];
                    stereo_buf[2 * i + 1] = out_buf[i];
                }
                wret = audio_pcm_write(stereo_buf, (size_t)fi.outputSamps);
            } else {
                continue;
            }
            if (wret != ESP_OK) {
                break;
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            break;
        } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue;
        } else {
            /* 解码失败（坏帧/假同步字）。MP3Decode 失败时 inbuf/bytesLeft 状态
               不一致：INVALID_FRAMEHEADER 不前进 inbuf（会在同一同步字死循环，
               饿死 IDLE1 触发 task_wdt）；INVALID_SIDEINFO 只前进 inbuf 不减
               bytesLeft（会越界）。故回退到解码前位置 + 1，跳过当前同步字，
               由循环顶部重新找下一个同步字。 */
            if (frame_bytes > 1) {
                read_ptr = frame_ptr + 1;
                bytes_left = frame_bytes - 1;
            } else {
                read_ptr = read_buf;
                bytes_left = 0;
            }
        }
    }

    MP3FreeDecoder(dec);
    fclose(f);
    audio_pcm_stop();
    ESP_LOGI(TAG, "play finished");
    mp3_player_finish();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t mp3_player_play(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 若正在播放/暂停，先停止旧曲并等待播放任务退出、释放资源
       （解码器 MP3FreeDecoder / 文件 fclose / I2S audio_pcm_stop 均在
       任务退出前完成），再启动新曲；最多等待 2s，超时视为失败。 */
    if (s_state != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (s_state != MP3_STATE_IDLE && wait < 2000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
        if (s_state != MP3_STATE_IDLE) {
            ESP_LOGE(TAG, "previous track stop timeout");
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "previous track stopped, resources released");
    }

    /* 重建暂停信号量：mp3_player_stop() 会 give 一次以唤醒暂停中的任务，
       播放中停止时该信号量会残留，删除重建避免新任务暂停时被立即误唤醒 */
    if (s_resume_sem != NULL) {
        vSemaphoreDelete(s_resume_sem);
    }
    s_resume_sem = xSemaphoreCreateBinary();
    if (s_resume_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_path = path;
    s_pause_requested = false;
    s_stop_requested = false;
    s_seek_requested = false;
    s_played_ms = 0;
    s_duration_s = 0;
    s_stopped_by_user = false;

    BaseType_t created = xTaskCreatePinnedToCore(play_task, "mp3_play",
                                                 MP3_PLAY_TASK_STACK, NULL,
                                                 MP3_PLAY_TASK_PRIO, NULL,
                                                 MP3_PLAY_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "create play task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mp3_player_pause(void)
{
    if (s_state != MP3_STATE_PLAYING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pause_requested = true;
    return ESP_OK;
}

esp_err_t mp3_player_resume(void)
{
    if (s_state != MP3_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_resume_sem);
    return ESP_OK;
}

esp_err_t mp3_player_stop(void)
{
    if (s_state == MP3_STATE_IDLE) {
        return ESP_OK;
    }
    s_stopped_by_user = true;   /* 标记用户主动停止，回调据此区分 */
    s_stop_requested = true;
    xSemaphoreGive(s_resume_sem);
    return ESP_OK;
}

esp_err_t mp3_player_seek(uint32_t target_s)
{
    if (s_state == MP3_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_seek_target_s = target_s;
    s_seek_requested = true;
    xSemaphoreGive(s_resume_sem);
    return ESP_OK;
}

mp3_player_state_t mp3_player_get_state(void)
{
    return s_state;
}

uint32_t mp3_player_get_position_s(void)
{
    if (s_state == MP3_STATE_PLAYING) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        return (s_played_ms + (now_ms - s_seg_start_ms)) / 1000;
    }
    return s_played_ms / 1000;
}

uint32_t mp3_player_get_duration_s(void)
{
    return s_duration_s;
}

void mp3_player_set_stopped_callback(mp3_player_stopped_cb_t cb)
{
    s_stopped_cb = cb;
}
