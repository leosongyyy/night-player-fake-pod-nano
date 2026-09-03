#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/eq.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3common.h"
#include "mp3dec.h"

static const char *TAG = "mp3_player";

/* 解码输入缓冲（内部 RAM 静态数组）：fread 一次补满，块读摊薄 SD 寻道。
   真正的抗碎片抖动由下面的 PCM ringbuf（PSRAM）承担，这里无需过大 */
#define MP3_READBUF_SIZE   (16 * 1024)

/* 输出缓冲：一帧解码的最大 PCM 样本数 = MAX_NCHAN(2) × MAX_NGRAN(2) × MAX_NSAMP(576) */
#define MP3_OUTBUF_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

/* 播放（producer）任务参数：读文件 + 解码 + 写 ringbuf */
#define MP3_PLAY_TASK_STACK 8192
#define MP3_PLAY_TASK_PRIO  8
#define MP3_PLAY_TASK_CORE  1

/* 输出（consumer）任务参数：从 ringbuf 取 PCM 写 I2S，优先级高于 producer
   保证音频不卡顿 */
#define MP3_OUT_TASK_STACK 4096
#define MP3_OUT_TASK_PRIO  9
#define MP3_OUT_TASK_CORE  1

/* PCM ringbuf（PSRAM，存 int16 交织 stereo，每帧 4 字节）：
   producer 解码抢跑写入、consumer 独立取出写 I2S，解耦读取/解码与输出。
   75KB ≈ 48kHz 下约 400ms，足以吸收碎片文件的 fread 延迟，又不至于让
   seek / 切歌时的排空等待过长 */
#define MP3_RINGBUF_SIZE   (75 * 1024)

/* 静态缓冲（BSS，不占任务栈）：
   read_buf 放内部 RAM（fread 目标，PSRAM 写入偏慢会加剧卡顿）；
   out_buf/stereo_buf 为解码输出与单声道→双声道转换缓冲 */
static unsigned char read_buf[MP3_READBUF_SIZE];
static short out_buf[MP3_OUTBUF_SAMPLES];
static short stereo_buf[MP3_OUTBUF_SAMPLES];   /* 单声道 → 双声道转换缓冲 */

/* producer/consumer 共享的 PCM ringbuf 与同步原语（首次播放时懒初始化） */
static RingbufHandle_t s_rb = NULL;
static TaskHandle_t s_out_task_handle = NULL;
static SemaphoreHandle_t s_clear_mutex = NULL;     /* 保护 ringbuf 清空与读的互斥 */
static SemaphoreHandle_t s_out_stopped_sem = NULL; /* consumer 停止确认（初始为空） */
static volatile bool s_out_running = false;        /* consumer 是否应运行 */

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

/* 待应用的"下一首"回放增益（由 mp3_player_set_gain 暂存，mp3_player_play
   在旧曲完全停止 + ringbuf 清空后才应用到 audio，避免旧曲残留 PCM 用
   新歌增益播放的音量突变） */
static uint32_t s_pending_gain = AUDIO_REPLAY_GAIN_UNITY;

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
        /* ⚠ 防御截断/损坏文件：跳过量不得超过文件实际大小。
           fseek 越过 EOF 本身不报错，但会使调用方按
           "文件长度 - data_offset" 计算音频长度时下溢成巨大值
           （损坏文件常见：ID3v2 头声明了远超实际长度的标签）。 */
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if ((long)skip > fsize) {
            ESP_LOGW(TAG, "ID3v2 tag larger than file (%u > %ld B), clamp",
                     (unsigned)skip, fsize);
            skip = (uint32_t)(fsize < 0 ? 0 : fsize);
        }
        fseek(f, (long)skip, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }
}

/* ------------------------------------------------------------------ */
/* 首帧有效性门禁（防御损坏/伪造文件进入解码器）                          */
/* ------------------------------------------------------------------ */

/* 校验 b 处是否为结构合法的 MPEG Layer3 帧头；合法返回该帧字节长度，否则 0。
   版本/表索引约定与 mp3_read_xing_duration 一致：verIdx=3(MPEG1)→ver=0、
   verIdx=2(MPEG2)→ver=1、verIdx=0(MPEG2.5)→ver=2（samplerateTab 等按此序）。 */
static uint32_t mp3_frame_header_len(const uint8_t *b)
{
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return 0;   /* 无同步字 0xFF Ex */
    }
    int verIdx = (b[1] >> 3) & 3;
    if (verIdx == 1) {
        return 0;   /* 保留版本 */
    }
    int ver = (verIdx == 0) ? 2 : ((verIdx & 1) ? 0 : 1);   /* 0=MPEG1 1=MPEG2 2=MPEG2.5 */
    int layer = 4 - ((b[1] >> 1) & 3);
    int brIdx = (b[2] >> 4) & 15;
    int srIdx = (b[2] >> 2) & 3;
    if (layer != 3 || brIdx == 0 || brIdx == 15 || srIdx == 3) {
        return 0;   /* 非 Layer3 / 自由码率 / 保留码率 / 保留采样率 */
    }
    int samprate = samplerateTab[ver][srIdx];
    int bitrate_kbps = bitrateTab[ver][layer - 1][brIdx];
    if (samprate <= 0 || bitrate_kbps <= 0) {
        return 0;
    }
    /* 帧长：MPEG1(ver=0) Layer3 = 144×码率/采样率，MPEG2/2.5(ver=1/2) = 72×码率/采样率 */
    uint32_t len = (ver == 0)
                   ? (144u * (uint32_t)bitrate_kbps * 1000u) / (uint32_t)samprate
                   : (72u * (uint32_t)bitrate_kbps * 1000u) / (uint32_t)samprate;
    return len + ((b[2] >> 1) & 1);   /* + padding 位 */
}

/* 在 buf[0..len) 中寻找"连续两个结构合法帧"的起点，找不到返回 -1。
   双帧连续性要求可排除随机数据的误判：坏文件（假 ID3v2 头 + 大段 0 +
   混杂其它文件字节）几乎不可能凑出两帧连续合法。 */
static int mp3_find_valid_frame_start(const uint8_t *buf, int len)
{
    for (int i = 0; i + 4 <= len; i++) {
        uint32_t flen = mp3_frame_header_len(buf + i);
        if (flen == 0) {
            continue;
        }
        if (flen > (uint32_t)(len - i)) {
            continue;   /* 首帧超出探测窗：无法验证第二帧，放弃该候选 */
        }
        if (mp3_frame_header_len(buf + i + (int)flen) != 0) {
            return i;   /* 两帧连续合法：确认真实音频数据起点 */
        }
    }
    return -1;
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
        /* 钳制到文件实际大小（防御声明过大的损坏标签） */
        long fsz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (fsz >= 0 && (long)offset > fsz) {
            offset = (uint32_t)fsz;
        }
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
/* PCM ringbuf 与输出任务（producer/consumer 解耦）                      */
/* ------------------------------------------------------------------ */

/* 清空 ringbuf（丢弃所有待播 PCM）。与 consumer 的读经 clear_mutex 互斥，
   seek / 播放开始前调用 */
static void ringbuf_clear(void)
{
    if (s_rb == NULL) {
        return;
    }
    xSemaphoreTake(s_clear_mutex, portMAX_DELAY);
    size_t item_size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
    xSemaphoreGive(s_clear_mutex);
}

/* ringbuf 是否已排空（无待读字节）。BYTEBUF 无 item 头开销 */
static bool ringbuf_is_empty(void)
{
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_rb, NULL, NULL, NULL, NULL, &waiting);
    return (waiting == 0);
}

/* 启动 consumer（置运行标志 + 唤醒等待中的输出任务） */
static void out_task_start(void)
{
    s_out_running = true;
    xTaskNotifyGive(s_out_task_handle);
}

/* 停止 consumer 并等待其确认（consumer 退出后停 I2S + 静音并 give 信号量）。
   consumer 最多在一个 I2S 写块（~30ms）+ 5ms 轮询内退出，200ms 超时足够 */
static void out_task_stop(void)
{
    if (!s_out_running) {
        return;
    }
    s_out_running = false;
    xSemaphoreTake(s_out_stopped_sem, pdMS_TO_TICKS(200));
}

/* 输出任务：独立从 ringbuf 取 PCM → 16bit 转 32bit slot → 写 I2S。
   与 producer 解耦，靠 ringbuf 吸收解码/读取的延迟抖动 */
static void out_task(void *arg)
{
    (void)arg;

    while (true) {
        /* 等待 producer 的启动通知 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (s_out_running) {
            xSemaphoreTake(s_clear_mutex, portMAX_DELAY);
            size_t item_size = 0;
            void *item = xRingbufferReceive(s_rb, &item_size, 0);   /* 非阻塞 */
            if (item != NULL) {
                /* item 为 int16 交织 stereo，帧数 = 字节 / 4 */
                size_t frames = item_size / (2 * sizeof(int16_t));
                audio_pcm_write((const int16_t *)item, frames);
                vRingbufferReturnItem(s_rb, item);
                xSemaphoreGive(s_clear_mutex);
            } else {
                xSemaphoreGive(s_clear_mutex);
                vTaskDelay(pdMS_TO_TICKS(5));   /* 空则小睡，避免忙等 */
            }
        }

        /* 停止：停 I2S + 静音，并通知 producer 已停 */
        audio_pcm_stop();
        xSemaphoreGive(s_out_stopped_sem);
    }
}

/* 懒初始化 producer/consumer 基础设施（首次播放时调用一次） */
static esp_err_t ensure_output_init(void)
{
    if (s_rb == NULL) {
        s_rb = xRingbufferCreateWithCaps(MP3_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rb == NULL) {
            ESP_LOGE(TAG, "create pcm ringbuf failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_clear_mutex == NULL) {
        s_clear_mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(s_clear_mutex);
    }
    if (s_out_stopped_sem == NULL) {
        s_out_stopped_sem = xSemaphoreCreateBinary();   /* 初始为空，等 consumer 停止时 give */
    }
    if (s_out_task_handle == NULL) {
        BaseType_t r = xTaskCreatePinnedToCore(out_task, "mp3_out",
                                               MP3_OUT_TASK_STACK, NULL,
                                               MP3_OUT_TASK_PRIO,
                                               &s_out_task_handle,
                                               MP3_OUT_TASK_CORE);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "create out task failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
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

    /* 音频数据长度（seek 字节映射与 CBR 兜底用）。
       ⚠ 防御截断/损坏文件：跳过量已钳制，此处再防 data_offset 落在 EOF 之后
       （文件比声明的 ID3v2 标签还短）时 uint32 下溢成巨大值 → 置 0 */
    fseek(f, 0, SEEK_END);
    long fend = ftell(f);
    uint32_t data_size = (fend > (long)data_offset)
                         ? (uint32_t)(fend - data_offset) : 0;
    fseek(f, (long)data_offset, SEEK_SET);

    /* 标签后没有任何音频数据（整文件就是损坏内容/只剩个假 ID3v2 头）：
       直接走"自然播完"收尾 → 控制器自动跳下一曲，避免对空数据解码 */
    if (data_size == 0) {
        ESP_LOGW(TAG, "no audio data after tag (%s), treat as damaged, skip",
                 s_path);
        fclose(f);
        mp3_player_finish();
        vTaskDelete(NULL);
        return;
    }

    /* ---- 首帧有效性门禁 ----
       真实 MP3 在 ID3 之后必然很快出现连续合法的音频帧；损坏文件（假 ID3v2、
       大段 0 填充、混杂其它文件数据的"伪文件"）几乎不可能凑出两个连续合法帧。
       在把数据交给 Helix 解码器之前先做结构性验证——实测此类坏曲直接进入
       解码会在解码出任何有效帧之前就破坏堆内存（UI 侧 LVGL 对象读到被
       FreeRTOS 栈填充字节 0xA5 覆盖的悬垂内存 → Core0 崩溃），门禁直接跳过
       该文件可彻底避免坏数据进入解码器。 */
    {
        int probe_len = (int)fread(read_buf, 1, sizeof(read_buf), f);
        if (probe_len <= 0) {
            ESP_LOGW(TAG, "read probe failed (%s), treat as damaged, skip",
                     s_path);
            fclose(f);
            mp3_player_finish();
            vTaskDelete(NULL);
            return;
        }
        int frame_off = mp3_find_valid_frame_start(read_buf, probe_len);
        if (frame_off < 0) {
            ESP_LOGW(TAG, "no valid mp3 frame at start (%s), damaged, skip",
                     s_path);
            fclose(f);
            mp3_player_finish();
            vTaskDelete(NULL);
            return;
        }
        if (frame_off > 0) {
            /* 首帧前有杂散字节（极少见）：从首帧位置开始（时长估算/seek 一致） */
            ESP_LOGW(TAG, "%d bytes junk before first frame, skip them",
                     frame_off);
        }
        data_offset += (uint32_t)frame_off;
        data_size -= (uint32_t)frame_off;
        fseek(f, (long)data_offset, SEEK_SET);   /* 解码循环从首帧处重新读 */
    }

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
            out_task_stop();               /* 停 consumer（停 I2S + 静音） */
            s_state = MP3_STATE_PAUSED;
            ESP_LOGI(TAG, "paused @ %u ms", (unsigned)s_played_ms);
            xSemaphoreTake(s_resume_sem, portMAX_DELAY);
            if (s_stop_requested) {
                s_stop_requested = false;
                break;
            }
            s_seg_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_state = MP3_STATE_PLAYING;
            ESP_LOGI(TAG, "resumed");
            /* 输出在 seek 检查后的统一逻辑里启动 */
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
            ringbuf_clear();   /* 丢弃跳转前尚未播出的旧 PCM */
            ESP_LOGI(TAG, "seek to %u s (byte %u)", (unsigned)target, (unsigned)byte_off);
        }

        /* ---- 统一确保输出在跑（pause 恢复 / 暂停中 seek 后） ---- */
        if (s_state == MP3_STATE_PLAYING && !s_out_running) {
            esp_err_t r = audio_pcm_start(s_fs);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "audio_pcm_start failed: %s", esp_err_to_name(r));
                break;
            }
            out_task_start();
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
        int bytes_before = bytes_left;   /* 记录调用前输入余量，检测"无进展" */
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
                s_state = MP3_STATE_PLAYING;
                started = true;
                ESP_LOGI(TAG, "mp3: %d Hz, %d ch, %d kbps, dur %u s",
                         fi.samprate, fi.nChans, fi.bitrate / 1000, (unsigned)s_duration_s);
                /* I2S 与 consumer 由循环顶部的统一逻辑在下一轮启动 */
            }

            /* 解码结果写 ringbuf（int16 交织 stereo；满则阻塞，形成背压） */
            if (fi.nChans == 2) {
                xRingbufferSend(s_rb, out_buf, (size_t)fi.outputSamps * sizeof(int16_t),
                                portMAX_DELAY);
            } else if (fi.nChans == 1) {
                for (int i = 0; i < fi.outputSamps; i++) {
                    stereo_buf[2 * i]     = out_buf[i];
                    stereo_buf[2 * i + 1] = out_buf[i];
                }
                xRingbufferSend(s_rb, stereo_buf,
                                (size_t)fi.outputSamps * 2 * sizeof(int16_t),
                                portMAX_DELAY);
            } else {
                continue;
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            break;
        } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            /* 主数据不足：正常时 MP3Decode 已吃掉帧头/side info（bytes_left 减少），
               继续循环取更多数据即可；但坏文件/假同步字可能出现反复 UNDERFLOW
               且输入完全不被消耗（bytes_left 不变）→ 无限空转，此时按坏帧处理，
               跳过该同步字后重新找同步。 */
            if (bytes_left < bytes_before) {
                continue;
            }
            /* fallthrough：无进展的 UNDERFLOW，同解码失败回退逻辑 */
            if (frame_bytes > 1) {
                read_ptr = frame_ptr + 1;
                bytes_left = frame_bytes - 1;
            } else {
                read_ptr = read_buf;
                bytes_left = 0;
            }
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

    /* 自然播完（非用户停止）时，先等 consumer 把 ringbuf 播空，避免截断结尾
       （ringbuf 里可能还攒着最多 ~400ms 的已解码 PCM）；用户主动停止则立即中断 */
    if (!s_stopped_by_user && s_out_running) {
        while (!ringbuf_is_empty() && !s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    out_task_stop();      /* 停 consumer 并等待其停 I2S，避免与下一曲启动竞争 */
    MP3FreeDecoder(dec);
    fclose(f);
    ringbuf_clear();      /* 丢弃残留 PCM */
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

    /* 懒初始化 producer/consumer 基础设施（首次播放时创建一次） */
    esp_err_t init = ensure_output_init();
    if (init != ESP_OK) {
        return init;
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

    /* 清空 ringbuf（丢弃上一曲可能残留的 PCM；此时 consumer 已停） */
    ringbuf_clear();

    /* ⚠ 旧曲已完全停止（consumer 停 I2S、ringbuf 已清空），此刻才把
       "下一首"的回放增益应用到 audio：此前若提前应用（如 play 前直接调
       audio_set_replay_gain），旧曲停止是异步的，停止等待期间旧曲残留
       PCM 仍会被送进 I2S，造成"旧歌内容用新歌增益播放"的音量突变 */
    audio_set_replay_gain(s_pending_gain);

    /* 同一静止点重置 EQ 滤波器状态（参数不变，仅清历史）：旧曲残留的
       滤波状态（x1/x2/y1/y2）不带入新歌，避免衔接处瞬态；
       EQ 系数在首帧解码出采样率后由 audio_pcm_start → audio_configure_fs
       按需重算（eq_notify_sample_rate），此处无需处理采样率 */
    eq_reset_state();

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

void mp3_player_set_gain(uint32_t gain_q16)
{
    /* 只暂存，下次 mp3_player_play 时（旧曲停止 + ringbuf 清空后）才应用 */
    s_pending_gain = gain_q16;
}
