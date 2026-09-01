#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/cs43131.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* ------------------------------------------------------------------ */
/* 引脚与 I2S 配置                                                      */
/* ------------------------------------------------------------------ */

#define AUDIO_I2S_NUM      I2S_NUM_0
#define AUDIO_BCK_PIN      GPIO_NUM_40   /* I2S 位时钟（主机输出） */
#define AUDIO_WS_PIN       GPIO_NUM_38   /* I2S 帧时钟 LRCK/WS（主机输出） */
#define AUDIO_DOUT_PIN     GPIO_NUM_39   /* I2S 数据输出 DIN */

#define AUDIO_TONE_TASK_STACK 4096
#define AUDIO_TONE_TASK_PRIO  8          /* 高于 LVGL 任务，保证音频不卡顿 */
#define AUDIO_TONE_TASK_CORE  1          /* LVGL 固定在 Core0，音频放 Core1 */

/* 正弦波查表：1024 点 int16，相位用 32 位定点累加器的高 10 位索引 */
#define SINE_TABLE_SIZE 1024

/* 播放缓冲：帧数（每帧 = 左右各 32bit slot），48kHz 下约 10.7ms/缓冲 */
#define AUDIO_BUF_FRAMES 512

/* I2S DMA 链参数（与 I2S_CHANNEL_DEFAULT_CONFIG 默认值一致）：
   dma_desc_num=6, dma_frame_num=240，每帧 = 左右 2 个 32bit slot = 8 字节。
   切换采样率后用一段静音 preload 覆盖整条 DMA 链，避免上一个采样率的残留数据
   以新速率被短暂播放（听起来像音调不对的极短正弦）。 */
#define I2S_DMA_DESC_NUM      6
#define I2S_DMA_FRAME_NUM     240
#define I2S_DMA_FLUSH_SAMPLES (I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM * 2)

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */

static i2s_chan_handle_t tx_chan = NULL;
static TaskHandle_t tone_task_handle = NULL;
static bool i2s_enabled = false;   /* I2S 通道是否已 enable（避免重复 disable 警告） */

static volatile bool playing = false;
static audio_fs_t cur_fs = AUDIO_FS_48000;
static uint16_t cur_freq = 0;
static uint8_t volume_level = AUDIO_VOL_LEVEL_DEFAULT;

static int16_t sine_table[SINE_TABLE_SIZE];

/* 静音冲刷缓冲：覆盖整条 I2S DMA 链（6×240×2 个 32bit 样本 = 11520 字节），
   放静态区（BSS），不占播放任务栈 */
static int32_t silence_flush[I2S_DMA_FLUSH_SAMPLES];

/* PCM 转换缓冲：int16 交织 → int32 左对齐，分块转换（512 帧 × 2ch × 4B = 4KB） */
static int32_t pcm_buf[AUDIO_BUF_FRAMES * 2];

/* ------------------------------------------------------------------ */
/* 正弦波查表生成（一次生成，正弦 16bit 满幅）                           */
/* ------------------------------------------------------------------ */

static void gen_sine_table(void)
{
    for (uint32_t i = 0; i < SINE_TABLE_SIZE; i++) {
        float v = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_TABLE_SIZE);
        sine_table[i] = (int16_t)(v * 32767.0f);
    }
}

/* ------------------------------------------------------------------ */
/* 播放任务：相位累加查表 → 32bit slot（16bit 数据左移 16 位）写 I2S      */
/* ------------------------------------------------------------------ */

static void tone_task(void *arg)
{
    /* 32 位定点相位步进：每采样点相位增量 = freq / fs × 2^32 */
    uint32_t step = (uint32_t)(((uint64_t)cur_freq << 32) / (uint32_t)cur_fs);
    uint32_t phase = 0;
    /* 缓冲放静态区而非栈上：512 帧 × 2ch × 4B = 4KB，避免任务栈溢出 */
    static int32_t buf[AUDIO_BUF_FRAMES * 2];

    ESP_LOGI(TAG, "tone task start: %u Hz @ %u Hz sample rate",
             (unsigned)cur_freq, (unsigned)cur_fs);

    while (playing) {
        for (uint32_t i = 0; i < AUDIO_BUF_FRAMES; i++) {
            int16_t s = sine_table[(phase >> 22) & (SINE_TABLE_SIZE - 1)];
            int32_t sample = ((int32_t)s) << 16;   /* 16bit 正弦左对齐到 32bit slot */
            buf[2 * i] = sample;                   /* 左声道 */
            buf[2 * i + 1] = sample;               /* 右声道（同相） */
            phase += step;
        }
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(tx_chan, buf, sizeof(buf), &written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    playing = false;
    tone_task_handle = NULL;
    ESP_LOGI(TAG, "tone task exit");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* I2S 初始化                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t i2s_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create i2s channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,      /* MCLK 由 CS43131 板载晶振提供 */
            .bclk = AUDIO_BCK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = AUDIO_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            /* CS43131 ASP 从机模式下 SCPOL_IN=1（SCLK 下降沿采样），
               ESP32 主机输出标准 I2S（Philips，不反相 BCLK/WS）与之匹配 */
            .invert_flags = {
                .bclk_inv = false,
                .mclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* 显式 32bit slot（CS43131 0x05000A/B=0x07/0x0F 配 32bit/sample） */
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init i2s std mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S channel ready (BCK=GPIO40, WS=GPIO38, DOUT=GPIO39)");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t audio_init(void)
{
    gen_sine_table();

    esp_err_t ret = i2s_audio_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = cs43131_init();
    if (ret != ESP_OK) {
        /* RCO 上电默认态不支持 DAC 播放（数据手册 4.6.1），I2C 不通即无声，直接报错 */
        ESP_LOGE(TAG, "cs43131 init failed (%s), audio unavailable", esp_err_to_name(ret));
        return ret;
    }

    ret = audio_set_volume_level(volume_level);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "audio init done (volume level %u/30)", (unsigned)volume_level);
    return ESP_OK;
}

/* 配置 CS43131 + I2S 到指定采样率（静音 → 切采样率 → 重配 I2S → 冲刷 DMA → 取消静音） */
static esp_err_t audio_configure_fs(audio_fs_t fs)
{
    /* 先静音防止切换采样率时爆音 */
    cs43131_set_mute(true);

    /* 切换 CS43131 ASP 采样率（仅写 0x01000B，不切换 MCLK，避免切换卡死） */
    esp_err_t ret = cs43131_set_sample_rate((uint32_t)fs);
    if (ret != ESP_OK) {
        cs43131_set_mute(false);
        return ret;
    }

    /* 重配 I2S 时钟（需在 disable 状态下）；32bit slot 用默认 256×fs MCLK */
    if (i2s_enabled) {
        i2s_channel_disable(tx_chan);
        i2s_enabled = false;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)fs);
    ret = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reconfig i2s clock failed: %s", esp_err_to_name(ret));
        cs43131_set_mute(false);
        return ret;
    }
    /* enable 前用静音覆盖 DMA 残留：preload 会从 desc[0] 起重新填充整条 DMA 链，
       使 enable 后第一轮送出的是静音而非上一个采样率的残留数据 */
    memset(silence_flush, 0, sizeof(silence_flush));
    size_t flushed = 0;
    i2s_channel_preload_data(tx_chan, silence_flush, sizeof(silence_flush), &flushed);
    i2s_channel_enable(tx_chan);
    i2s_enabled = true;

    cs43131_set_mute(false);

    cur_fs = fs;
    return ESP_OK;
}

esp_err_t audio_play_tone(audio_fs_t fs, uint16_t freq_hz)
{
    if (fs != AUDIO_FS_32000 && fs != AUDIO_FS_44100 && fs != AUDIO_FS_48000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (freq_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 正在播放则先停止 */
    if (playing) {
        audio_stop();
    }

    esp_err_t ret = audio_configure_fs(fs);
    if (ret != ESP_OK) {
        return ret;
    }

    cur_freq = freq_hz;
    playing = true;

    BaseType_t created = xTaskCreatePinnedToCore(tone_task, "audio_tone",
                                                 AUDIO_TONE_TASK_STACK, NULL,
                                                 AUDIO_TONE_TASK_PRIO,
                                                 &tone_task_handle,
                                                 AUDIO_TONE_TASK_CORE);
    if (created != pdPASS) {
        playing = false;
        ESP_LOGE(TAG, "create tone task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "playing %u Hz sine @ %u Hz", freq_hz, (unsigned)fs);
    return ESP_OK;
}

esp_err_t audio_stop(void)
{
    if (!playing && tone_task_handle == NULL) {
        return ESP_OK;
    }

    playing = false;

    /* 等待播放任务退出（最多 ~100ms，i2s write 阻塞解除后即退出） */
    uint32_t wait = 0;
    while (tone_task_handle != NULL && wait < 100) {
        vTaskDelay(pdMS_TO_TICKS(2));
        wait += 2;
    }

    if (i2s_enabled) {
        i2s_channel_disable(tx_chan);
        i2s_enabled = false;
    }
    cs43131_set_mute(true);   /* 停止后静音 DAC，避免残留噪声 */

    ESP_LOGI(TAG, "audio stopped");
    return ESP_OK;
}

esp_err_t audio_pcm_start(audio_fs_t fs)
{
    if (fs != AUDIO_FS_32000 && fs != AUDIO_FS_44100 && fs != AUDIO_FS_48000) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 若正弦正在播放则先停止 */
    if (playing) {
        audio_stop();
    }

    return audio_configure_fs(fs);
}

esp_err_t audio_pcm_write(const int16_t *samples, size_t frames)
{
    if (samples == NULL || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int16_t *src = samples;
    size_t remain = frames;
    while (remain > 0) {
        size_t chunk = remain > AUDIO_BUF_FRAMES ? AUDIO_BUF_FRAMES : remain;
        /* 16bit 交织 → 32bit slot 左对齐（MSB 对齐，匹配 CS43131 32bit slot） */
        for (size_t i = 0; i < chunk; i++) {
            pcm_buf[2 * i]     = ((int32_t)src[2 * i]) << 16;
            pcm_buf[2 * i + 1] = ((int32_t)src[2 * i + 1]) << 16;
        }
        size_t bytes = chunk * 2 * sizeof(int32_t);
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(tx_chan, pcm_buf, bytes, &written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "pcm write failed: %s", esp_err_to_name(ret));
            return ret;
        }
        src += chunk * 2;
        remain -= chunk;
    }
    return ESP_OK;
}

esp_err_t audio_pcm_stop(void)
{
    if (i2s_enabled) {
        i2s_channel_disable(tx_chan);
        i2s_enabled = false;
    }
    cs43131_set_mute(true);   /* 停止后静音 DAC */
    return ESP_OK;
}

/* 音量档位 → 百分比：1~30 线性映射 20%~80%（整数运算，端点到点：
   档 1 = 20%，档 30 = 80%，每档约 2.07 个百分点） */
static uint8_t vol_level_to_percent(uint8_t level)
{
    if (level <= AUDIO_VOL_LEVEL_MUTE) {
        return 0;
    }
    if (level >= AUDIO_VOL_LEVEL_MAX) {
        return AUDIO_VOL_PERCENT_MAX;
    }
    return (uint8_t)(AUDIO_VOL_PERCENT_MIN +
                     ((uint32_t)(level - AUDIO_VOL_LEVEL_MIN)
                      * (AUDIO_VOL_PERCENT_MAX - AUDIO_VOL_PERCENT_MIN)
                      / (AUDIO_VOL_LEVEL_MAX - AUDIO_VOL_LEVEL_MIN)));
}

esp_err_t audio_set_volume_level(uint8_t level)
{
    if (level > AUDIO_VOL_LEVEL_MAX) {
        level = AUDIO_VOL_LEVEL_MAX;
    }
    volume_level = level;

    /* CS43131 音量 0x00 = 0dB（最大），0xFF = 最小。
       0 档静音：直接写最小音量 0xFF（原配置下 <10 已几乎无声，不走静音控制位） */
    uint8_t reg = 0xFF;
    if (level > AUDIO_VOL_LEVEL_MUTE) {
        uint8_t percent = vol_level_to_percent(level);
        reg = (uint8_t)(255u - ((uint32_t)percent * 255u / 100u));
    }
    return cs43131_set_volume(reg);
}

bool audio_is_playing(void)
{
    return playing;
}

audio_fs_t audio_get_fs(void)
{
    return cur_fs;
}

uint16_t audio_get_freq(void)
{
    return cur_freq;
}

uint8_t audio_get_volume_level(void)
{
    return volume_level;
}
