#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/bt_audio.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/eq.h"
#include "core/storage/config_store.h"

#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* ------------------------------------------------------------------ */
/* 引脚与 I2S 配置                                                      */
/* ------------------------------------------------------------------ */

/* ESP32-S31：DAC 走 I2S1（I2S0 留给 PDM 麦克风，见 hardware_v1.md §6/§7） */
#define AUDIO_I2S_NUM      I2S_NUM_1
#define AUDIO_BCK_PIN      GPIO_NUM_52   /* I2S1 位时钟（主机输出） */
#define AUDIO_WS_PIN       GPIO_NUM_51   /* I2S1 帧时钟 LRCK/WS（主机输出） */
#define AUDIO_DOUT_PIN     GPIO_NUM_50   /* I2S1 数据输出 DIN */

/* 播放缓冲：帧数（每帧 = 左右各 32bit slot），48kHz 下约 10.7ms/缓冲 */
#define AUDIO_BUF_FRAMES 512

/* I2S DMA 链参数：dma_desc_num=8, dma_frame_num=511（约 93ms @44.1k）。
   每帧 = 左右 2 个 32bit slot = 8 字节。511 而不是 512：单描述符缓冲区上限
   DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED = 4092 字节，512×8=4096 会被
   I2S 驱动截断并告警，511×8=4088 是安全上限。
   切换采样率后用一段静音 preload 覆盖整条 DMA 链，避免上一个采样率的残留数据
   以新速率被短暂播放（听起来像音调不对的极短正弦）。 */
#define I2S_DMA_DESC_NUM      8
#define I2S_DMA_FRAME_NUM     511
#define I2S_DMA_FLUSH_SAMPLES (I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM * 2)

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */

static i2s_chan_handle_t tx_chan = NULL;
static bool i2s_enabled = false;   /* I2S 通道是否已 enable（避免重复 disable 警告） */

/* audio_shutdown() 之后为 true：DAC 已掉电，音量/静音/写 I2S 都不应再发生 */
static bool s_shutdown = false;

/* 停 PCM 前留给软斜坡静音的时间（ms）：DAC 的 PCM 音量是渐变静音，
   抽掉 I2S 时钟前先让它降下去，避免"咔哒"声。30ms 足够斜坡走完大半，
   又不会让切歌/停止明显变慢（关机路径另有更完整的 150ms，见 audio_shutdown） */
#define AUDIO_STOP_MUTE_WAIT_MS  30

static audio_fs_t cur_fs = AUDIO_FS_48000;
static uint8_t volume_level = AUDIO_VOL_LEVEL_DEFAULT;

/* ---- 蓝牙输出路由（2026-11） ----
   s_bt_active     : 音频输出当前是否走蓝牙（true 时 audio_pcm_write 全部进
                     bt_audio，DAC 已静音/停 I2S）；
   s_profile_active: 是否处于"蓝牙设备配置优先"状态（音量/EQ 来自设备记录，
                     不写 config_store）；
   s_pcm_running   : 当前是否处于一次播放会话（audio_pcm_start 之后、stop 之前）。
                     断开蓝牙回退 DAC 时用它判断要不要重新拉 I2S。 */
static volatile bool s_bt_active = false;
static volatile bool s_profile_active = false;
static volatile bool s_pcm_running = false;

/* 回放增益（16.16 定点线性系数，65536 = 0dB）：由上层按歌曲元数据设置。
   uint32 原子读（32bit 平台），consumer 每块读一次转 float 使用 */
static uint32_t replay_gain = AUDIO_REPLAY_GAIN_UNITY;

/* 静音冲刷缓冲：覆盖整条 I2S DMA 链（8×511×2 个 32bit 样本 = 32704 字节）。
   2026-11 起放 **PSRAM**（蓝牙接入后内部 DMA 池紧张；它只在切换采样率时
   被 memset 一次、由 i2s_channel_preload_data 拷进内部 DMA 描述符，
   不参与实时 DMA，PSRAM 带宽足够）。 */
static int32_t *silence_flush = NULL;

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
    /* 必须在 i2s_new_channel 之前覆盖默认 6×240，否则改宏不生效 */
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
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

    i2s_chan_info_t chan_info = {0};
    if (i2s_channel_get_info(tx_chan, &chan_info) == ESP_OK) {
        ESP_LOGI(TAG, "I2S DMA: %d x %d frames, total %u bytes",
                 I2S_DMA_DESC_NUM, I2S_DMA_FRAME_NUM,
                 (unsigned)chan_info.total_dma_buf_size);
    }
    ESP_LOGI(TAG, "I2S channel ready (I2S1 BCK=GPIO52, WS=GPIO51, DOUT=GPIO50)");
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

    /* 采样率切换用的静音冲刷缓冲（PSRAM；失败回退内部 RAM） */
    silence_flush = heap_caps_malloc(I2S_DMA_FLUSH_SAMPLES * sizeof(int32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (silence_flush == NULL) {
        silence_flush = heap_caps_malloc(I2S_DMA_FLUSH_SAMPLES * sizeof(int32_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (silence_flush == NULL) {
        ESP_LOGW(TAG, "alloc silence flush buffer failed, skip DMA preload");
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

    /* 蓝牙输出链路（重采样器 + PCM ringbuf；不启动蓝牙栈，只准备缓冲）。
       失败不致命：没有蓝牙时本机 DAC 播放照常工作。 */
    if (bt_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "bt audio init failed, bluetooth output unavailable");
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
if (silence_flush != NULL) {
        size_t flush_bytes = I2S_DMA_FLUSH_SAMPLES * sizeof(int32_t);
        memset(silence_flush, 0, flush_bytes);
        size_t flushed = 0;
        i2s_channel_preload_data(tx_chan, silence_flush, flush_bytes, &flushed);
        if (flushed != flush_bytes) {
            ESP_LOGW(TAG, "silence preload %u/%u bytes",
                     (unsigned)flushed, (unsigned)flush_bytes);
        }
    }
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

    /* 蓝牙输出：不配置 I2S/DAC，只把源采样率交给重采样器，并请求 A2DP 起流。
       ⚠ EQ 在蓝牙路径里是"重采样之后"按输出 fs 处理的（需求明确要求），
       所以这里通知的是蓝牙输出采样率而不是源采样率。 */
    if (s_bt_active) {
        cur_fs = fs;
        s_pcm_running = true;
        bt_audio_set_source_fs((uint32_t)fs);
        uint32_t out_fs = bt_audio_get_out_fs();
        if (out_fs != 0) {
            eq_notify_sample_rate(out_fs);
        }
        bt_audio_request_stream(true);
        return ESP_OK;
    }

    esp_err_t ret = audio_configure_fs(fs);
    if (ret == ESP_OK) {
        s_pcm_running = true;
    }
    return ret;
}

esp_err_t audio_pcm_write(const int16_t *samples, size_t frames)
{
    if (samples == NULL || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 蓝牙输出：整条 DSP 链（重采样 → EQ → 增益）在 bt_audio 里完成 */
    if (s_bt_active) {
        bt_audio_write(samples, frames);
        return ESP_OK;
    }

    /* 回放增益 float（uint32 原子读）：q16 → 线性（注意无符号增益 × 有符号
       样本的符号转换统一在 float 域完成，见 audio.h 处理链路说明）。
       增益为直接切换（无平滑过渡）：切歌时旧任务已完全退出，新曲在开始
       写 PCM 前应用增益，不存在跨曲混用。 */
    const uint32_t gain_q16 = replay_gain;
    const float gain_f = (float)gain_q16 * (1.0f / 65536.0f);

    const int16_t *src = samples;
    size_t remain = frames;
    while (remain > 0) {
        size_t chunk = remain > AUDIO_BUF_FRAMES ? AUDIO_BUF_FRAMES : remain;
        /* EQ 快照对齐（版本一致时零拷贝）：保证本块使用一致参数；滤波器
           状态在 s_eq_rt 内跨块延续（eq_apply/切歌等参数版本变化时才会
           整体重载并清零状态） */
        eq_snapshot(&s_eq_rt);

        if (s_eq_rt.band_count == 0 && gain_q16 == AUDIO_REPLAY_GAIN_UNITY) {
            /* 整数快路径：与下面浮点路径逐位等价。band_count=0 时 EQ 直通且
               不乘预增益；unity 增益精确为 1.0f；int16/32768 × 2^31 = int16×65536，
               结果在 int32 范围内，不需要 Clamp。用乘法避免负数左移的 UB。 */
            for (size_t i = 0; i < chunk * 2; i++) {
                pcm_buf[i] = (int32_t)src[i] * 65536;
            }
        } else {
            /* 统一浮点路径（EQ 关闭但增益非 1.0 时也走这里）：
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
    /* 蓝牙输出：请求 A2DP 挂起并丢弃缓冲，不需要碰 DAC/I2S */
    if (s_bt_active) {
        s_pcm_running = false;
        bt_audio_request_stream(false);
        bt_audio_flush();
        return ESP_OK;
    }

    s_pcm_running = false;
    /* 先软斜坡静音，再停时钟：旧顺序（先 i2s_disable 再 mute）会在 DAC 仍有
       输出电平时抽掉 BCK/LRCK，耳机里出现可闻的"咔哒"声 */
    cs43131_set_mute(true);
    if (i2s_enabled) {
        vTaskDelay(pdMS_TO_TICKS(AUDIO_STOP_MUTE_WAIT_MS));
        i2s_channel_disable(tx_chan);
        i2s_enabled = false;
    }
    return ESP_OK;
}

/* 停 I2S 时钟（幂等；调用方须确保 DAC 已静音/掉电，否则会有咔哒声） */
static void audio_i2s_stop(void)
{
    if (i2s_enabled) {
        i2s_channel_disable(tx_chan);
        i2s_enabled = false;
    }
}

void audio_shutdown(void)
{
    if (s_shutdown) {
        return;
    }
    s_shutdown = true;

    ESP_LOGI(TAG, "audio shutdown: pop-free DAC power down");

    /* 1. CS43131 pop-free 掉电：软斜坡静音 → 150ms → 关耳放 → 关 ASP →
          恢复默认 → 关晶振 → RESET# 拉低。
          ⚠ **必须在停 I2S 之前**：软斜坡是数字音量渐变，需要 MCLK/ASP 时钟
          在跑；先停时钟再静音等于没有斜坡，反而更容易爆音。 */
    esp_err_t ret = cs43131_power_off();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "cs43131 power off failed: %s", esp_err_to_name(ret));
    }

    /* 2. 最后停 I2S 时钟（DAC 已在复位态，抽掉时钟不会出声） */
    audio_i2s_stop();

    ESP_LOGI(TAG, "audio shutdown done");
}

esp_err_t audio_recover_after_failed_shutdown(void)
{
    if (!s_shutdown) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "shutdown aborted: re-initializing CS43131 / audio");

    /* CS43131 被 RESET# 复位过：重新走一遍复位 + 完整配置（音量寄存器不在
       配置序列里，随后单独恢复）。I2S 通道对象仍在，下一次 audio_pcm_start()
       会重新配置时钟并 enable，此处无需处理。 */
    esp_err_t ret = cs43131_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio recover failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_shutdown = false;
    return audio_set_volume_level(volume_level);
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
    /* 蓝牙路径同样需要回放增益（在 EQ 之后、软件音量之前相乘） */
    bt_audio_set_gain_q16(gain_q16);
    ESP_LOGI(TAG, "replay gain set: %u.%04u (x%.3f)",
             gain_q16 >> 16, (unsigned)(gain_q16 & 0xFFFF),
             (double)gain_q16 / 65536.0);
}

/* 音量档位 → 百分比：1~30 线性映射 35%~85%（整数运算，端点到点：
   档 1 = 35%，档 30 = 85%）。蓝牙绝对音量换算也复用它（见 audio.h）。 */
uint8_t audio_volume_level_to_percent(uint8_t level)
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

/* 蓝牙软件音量曲线（耳机**不支持**绝对音量时用）：**线性**映射 1..30 →
 * 1/30..1.0（0 = 静音）。⚠ 与 DAC 的 35%~85% 曲线无关（那条是给 CS43131
 * 特调的）；支持绝对音量时本函数不参与（软件增益固定 1.0，音量交给耳机）。 */
static float bt_vol_level_to_soft_gain(uint8_t level)
{
    if (level <= AUDIO_VOL_LEVEL_MUTE) {
        return 0.0f;
    }
    if (level > AUDIO_VOL_LEVEL_MAX) {
        level = AUDIO_VOL_LEVEL_MAX;
    }
    return (float)level / (float)AUDIO_VOL_LEVEL_MAX;
}

/* 把当前音量档位应用到**当前输出**：
   - 蓝牙 + 耳机支持绝对音量：本机软件增益 1.0，发 0~127 给耳机；
   - 蓝牙 + 不支持：本机软件衰减（平方曲线）；
   - DAC：写 CS43131 音量寄存器（0xFF 最小 = 静音档）。 */
static esp_err_t audio_apply_volume_to_output(void)
{
    if (s_bt_active) {
        if (bt_audio_absvol_supported()) {
            bt_audio_set_soft_gain(1.0f);
            bt_audio_send_absvol(volume_level);
        } else {
            bt_audio_set_soft_gain(bt_vol_level_to_soft_gain(volume_level));
        }
        return ESP_OK;
    }

    /* CS43131 音量 0x00 = 0dB（最大），0xFF = 最小。
       0 档静音：直接写最小音量 0xFF（原配置下 <10 已几乎无声，不走静音控制位） */
    uint8_t reg = 0xFF;
    if (volume_level > AUDIO_VOL_LEVEL_MUTE) {
        uint8_t percent = audio_volume_level_to_percent(volume_level);
        reg = (uint8_t)(255u - ((uint32_t)percent * 255u / 100u));
    }
    return cs43131_set_volume(reg);
}

esp_err_t audio_set_volume_level(uint8_t level)
{
    if (level > AUDIO_VOL_LEVEL_MAX) {
        level = AUDIO_VOL_LEVEL_MAX;
    }
    volume_level = level;
    return audio_apply_volume_to_output();
}

audio_fs_t audio_get_fs(void)
{
    return cur_fs;
}

/* ------------------------------------------------------------------ */
/* 蓝牙输出路由（2026-11）                                               */
/* ------------------------------------------------------------------ */

bool audio_bt_profile_active(void)
{
    return s_profile_active;
}

void audio_bt_profile_enter(uint8_t volume, const eq_preset_t *eq)
{
    if (volume > AUDIO_VOL_LEVEL_MAX) {
        volume = AUDIO_VOL_LEVEL_MAX;
    }
    s_profile_active = true;
    volume_level = volume;
    /* 先用当前 s_fs 预计算 EQ 系数；真正切到蓝牙输出后 audio_bt_output_enable
       会按蓝牙输出采样率重算（eq_notify_sample_rate），满足"EQ 在重采样之后" */
    eq_apply(eq);
    ESP_LOGI(TAG, "BT profile enter: volume %u/30, EQ %s", (unsigned)volume,
             (eq != NULL && eq->band_count > 0) ? "on" : "off");
}

void audio_bt_profile_leave(void)
{
    s_profile_active = false;
    volume_level = config_store_get_volume();
    audio_apply_volume_to_output();

    /* 恢复配置槽位的 EQ（设备 EQ 只在该设备连接期间生效） */
    char name[EQ_FILE_NAME_MAX];
    eq_preset_t preset;
    if (config_store_get_eq(name, sizeof(name), &preset)) {
        eq_apply(&preset);
        ESP_LOGI(TAG, "BT profile leave: restore slot EQ %s", name);
    } else {
        eq_apply(NULL);
        ESP_LOGI(TAG, "BT profile leave: restore slot volume %u, no EQ",
                 (unsigned)volume_level);
    }
}

void audio_bt_output_enable(uint32_t out_fs)
{
    if (out_fs == 0) {
        return;
    }
    if (s_bt_active) {
        audio_bt_set_out_fs(out_fs);
        return;
    }

    /* 先把 DAC 侧安静地停掉（软斜坡静音 → 停时钟），再切路由。
       正在播放的状态要保留：切到蓝牙后由 bt_service 请求 A2DP 起流。 */
    const bool was_running = s_pcm_running;
    audio_pcm_stop();              /* s_pcm_running 被清，下面恢复 */

    s_bt_active = true;
    bt_audio_output_start((uint32_t)cur_fs, out_fs);
    eq_notify_sample_rate(out_fs); /* EQ 系数以蓝牙输出采样率为基准 */
    audio_apply_volume_to_output();
    s_pcm_running = was_running;
    if (was_running) {
        bt_audio_request_stream(true);   /* 正在播放：请求 A2DP 起流 */
    }

    ESP_LOGI(TAG, "audio output -> BT (%u Hz), DAC muted%s",
             (unsigned)out_fs, was_running ? ", resume stream" : "");
}

void audio_bt_output_disable(void)
{
    if (!s_bt_active) {
        return;
    }
    const bool was_running = s_pcm_running;
    s_bt_active = false;
    bt_audio_output_stop();

    if (was_running) {
        /* 回到 DAC 继续播：重新配置采样率/I2S，EQ 系数也切回源采样率 */
        audio_pcm_start(cur_fs);
    } else {
        audio_apply_volume_to_output();
    }
    ESP_LOGI(TAG, "audio output -> DAC%s", was_running ? " (resume)" : "");
}

void audio_bt_set_out_fs(uint32_t out_fs)
{
    if (!s_bt_active || out_fs == 0) {
        return;
    }
    bt_audio_set_out_fs(out_fs);
    eq_notify_sample_rate(out_fs);
    ESP_LOGI(TAG, "BT output fs -> %u Hz", (unsigned)out_fs);
}

void audio_bt_refresh_volume(void)
{
    audio_apply_volume_to_output();
}

void audio_volume_from_remote(uint8_t level)
{
    if (level > AUDIO_VOL_LEVEL_MAX) {
        level = AUDIO_VOL_LEVEL_MAX;
    }
    volume_level = level;
    if (s_bt_active) {
        /* 耳机侧已经改了自己的音量：不要回发绝对音量命令（避免打架），
           只把本机软件增益保持一致（支持绝对音量时保持 0dB） */
        if (bt_audio_absvol_supported()) {
            bt_audio_set_soft_gain(1.0f);
        } else {
            bt_audio_set_soft_gain(bt_vol_level_to_soft_gain(level));
        }
    } else {
        audio_apply_volume_to_output();
    }
    ESP_LOGI(TAG, "volume from remote: %u/30", (unsigned)level);
}

uint8_t audio_get_volume_level(void)
{
    return volume_level;
}
