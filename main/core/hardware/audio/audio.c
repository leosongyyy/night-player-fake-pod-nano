#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/eq.h"

#include <stdint.h>
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
static bool i2s_enabled = false;   /* I2S 通道是否已 enable（避免重复 disable 警告） */

static audio_fs_t cur_fs = AUDIO_FS_48000;
static uint8_t volume_level = AUDIO_VOL_LEVEL_DEFAULT;

/* 回放增益（16.16 定点线性系数，65536 = 0dB）：由上层按歌曲元数据设置。
   uint32 原子读（32bit 平台），consumer 每块读一次转 float 使用 */
static uint32_t replay_gain = AUDIO_REPLAY_GAIN_UNITY;

/* 静音冲刷缓冲：覆盖整条 I2S DMA 链（6×240×2 个 32bit 样本 = 11520 字节），
   放静态区（BSS），不占播放任务栈 */
static int32_t silence_flush[I2S_DMA_FLUSH_SAMPLES];

/* PCM 转换缓冲：float 处理结果 → int32 左对齐，分块转换（512 帧 × 2ch × 4B = 4KB） */
static int32_t pcm_buf[AUDIO_BUF_FRAMES * 2];

/* EQ 运行时（BSS，全零 = 直通且 ver=0 与内部一致）：consumer 侧持续持有，
   滤波器状态在块间演化延续（勿让任何"每块复制"覆盖它，否则周期瞬态）；
   eq_snapshot 仅在参数版本变化时整体重载（见 eq.h） */
static eq_runtime_t s_eq_rt;

/* float 归一化常量：int16 满幅 32768 → ±1.0 */
#define AUDIO_INV_32768    (1.0f / 32768.0f)

/* 输出缩放：归一化样本 × 2^31 即 16bit 内容左移 16 位后的 int32 域
   （x = ±1.0 → ±0x80000000，与旧整数路径 s×q16 满幅语义一致） */
#define AUDIO_S32_SCALE    2147483648.0f   /* 2^31，float 可精确表示 */

/* float → int32 左对齐样本：先 Clamp 到 int32 值域再转换（越界 float→int
   转换在 C 中是未定义行为；Clamp 同时防止 EQ/增益叠加超幅回绕爆音） */
static inline int32_t audio_f32_to_s32(float v)
{
    if (v >= AUDIO_S32_SCALE) {
        return INT32_MAX;
    }
    if (v <= -AUDIO_S32_SCALE) {
        return INT32_MIN;
    }
    return (int32_t)v;
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

    /* EQ 滤波器系数依赖采样率：采样率变化时按当前激活预设重算（调用点在
       consumer 启动前，无样本处理并发） */
    eq_notify_sample_rate((uint32_t)fs);

    cur_fs = fs;
    return ESP_OK;
}

esp_err_t audio_pcm_start(audio_fs_t fs)
{
    if (fs != AUDIO_FS_32000 && fs != AUDIO_FS_44100 && fs != AUDIO_FS_48000) {
        return ESP_ERR_INVALID_ARG;
    }

    return audio_configure_fs(fs);
}

esp_err_t audio_pcm_write(const int16_t *samples, size_t frames)
{
    if (samples == NULL || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 回放增益 float（uint32 原子读）：q16 → 线性（注意无符号增益 × 有符号
       样本的符号转换统一在 float 域完成，见 audio.h 处理链路说明）。
       增益为直接切换（无平滑过渡）：切歌在"旧曲已停 + ringbuf 已清"的
       静止点应用，回放增益开关切换也只在队列空闲时应用，不存在跨曲混用。 */
    const float gain_f = (float)replay_gain * (1.0f / 65536.0f);

    const int16_t *src = samples;
    size_t remain = frames;
    while (remain > 0) {
        size_t chunk = remain > AUDIO_BUF_FRAMES ? AUDIO_BUF_FRAMES : remain;
        /* EQ 快照对齐（版本一致时零拷贝）：保证本块使用一致参数；滤波器
           状态在 s_eq_rt 内跨块延续（eq_apply/切歌等参数版本变化时才会
           整体重载并清零状态） */
        eq_snapshot(&s_eq_rt);

        /* 统一浮点路径（EQ 关闭时 band_count=0，eq_process_sample 直通）：
           int16 → float 归一化 → EQ（预增益+级联）→ ×回放增益 → Clamp →
           int32 左对齐。EQ/增益叠加超幅的样本在此被 Clamp 而非回绕 */
        for (size_t i = 0; i < chunk; i++) {
            float xl = eq_process_sample(&s_eq_rt, 0,
                                         (float)src[2 * i] * AUDIO_INV_32768);
            float xr = eq_process_sample(&s_eq_rt, 1,
                                         (float)src[2 * i + 1] * AUDIO_INV_32768);
            pcm_buf[2 * i]     = audio_f32_to_s32(xl * gain_f * AUDIO_S32_SCALE);
            pcm_buf[2 * i + 1] = audio_f32_to_s32(xr * gain_f * AUDIO_S32_SCALE);
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

void audio_set_replay_gain(uint32_t gain_q16)
{
    /* 增益钳位到 [0.25x, 4x]（16.16 定点 16384 ~ 262144）：过小的值几近
       无声（异常数据），过大必然严重削波，两端都无实际意义 */
    if (gain_q16 == 0 || gain_q16 > 262144u) {
        gain_q16 = AUDIO_REPLAY_GAIN_UNITY;
    } else if (gain_q16 < 16384u) {
        gain_q16 = 16384u;
    }
    replay_gain = gain_q16;
    ESP_LOGI(TAG, "replay gain set: %u.%04u (x%.3f)",
             gain_q16 >> 16, (unsigned)(gain_q16 & 0xFFFF),
             (double)gain_q16 / 65536.0);
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

audio_fs_t audio_get_fs(void)
{
    return cur_fs;
}

uint8_t audio_get_volume_level(void)
{
    return volume_level;
}
