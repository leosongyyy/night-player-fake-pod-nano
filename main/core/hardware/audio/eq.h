#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file eq.h
 * @brief 均衡器（10 段浮点 PeakEQ 级联 + 预增益），.eq 文件与 NightPlayer 兼容
 *
 * 对应 ideas_and_exps/FUTURE_PLAN.md 第 10 点：
 *   - 参考实现：Desktop/Ctyx（床头音响）的 biquad_filter（RBJ Peaking EQ）；
 *   - .eq 文件存放于 SD 卡根目录 .musicfx 文件夹：第一行预增益（0~1 线性），
 *     之后每行三段参数 fCenter(16~20000)/fQ 的倒数(0.2~5)/dBGain(±15)，
 *     数字用逗号分隔、允许空格；空行忽略、无效行判整个文件无效；
 *     最多 10 段，超过视为无效（UI 弹 Toast 提示）；
 *   - 解析结果（eq_preset_t，纯数据）由上层保存到 NVS（config_store）并在
 *     开机恢复（参数数据入 NVS，不依赖 SD 卡上的 .eq 文件仍在）；
 *   - DSP：int16 → float(÷32768 归一化) → ×预增益 → 10 段 PeakEQ 级联
 *     （每声道一条链）→ ×回放增益（float）→ Clamp → int32 左对齐输出
 *     （audio.c audio_pcm_write 中执行）。
 *
 * 线程安全设计：
 *   - 激活状态（eq_runtime_t）可能在播放中由 UI（Core0）更新，而样本处理在
 *     audio_pcm_write（Core1 的 mp3_out 任务）中执行 → 参数/状态整体替换与
 *     快照读取均走临界区（portMUX）；系数预计算（含 pow/cos/sin，较慢）在
 *     临界区外完成，临界区内仅做 memcpy（~1us），播放中切换 EQ 不爆音不撕裂；
 *   - 滤波器系数与采样率相关：audio_configure_fs 在采样率配置完成点调用
 *     eq_notify_sample_rate() 重算（此时 consumer 未启动，无并发）；
 *   - 切歌时由 mp3_player 在"旧曲停止 + ringbuf 清空"静止点调用
 *     eq_reset_state() 清零滤波器状态（参数不变），避免旧曲状态污染新歌。
 */

/** 单条 EQ 最大段数（.eq 文件超过则判无效） */
#define EQ_MAX_BANDS 10

/** .eq 文件名最大长度（含 ".eq" 扩展名与结尾 '\0'） */
#define EQ_FILE_NAME_MAX 64

/** EQ 参数一行：一段 PeakEQ 的原始参数（.eq 文件"fCenter,fQ倒数,dBGain"） */
typedef struct {
    float center;   /* 中心频率 Hz（合法范围 16~20000） */
    float inv_q;    /* fQ 的倒数（合法范围 0.2~5，即 Q = 1/inv_q ∈ 0.2~5） */
    float gain_db;  /* 峰值增益 dB（合法范围 -15~+15） */
} eq_band_param_t;

/**
 * @brief 一个 EQ 预设（.eq 文件解析结果，纯数据无指针，可整块 memcpy/NVS）
 *
 * band_count = 0 表示无 EQ（直通，不乘预增益）；字段固定布局，大小
 * = 1+3(pad)+4+10*12 = 128 字节（见 sizeof 断言），NVS blob 直存。
 */
typedef struct {
    uint8_t band_count;                  /* 有效段数（0~EQ_MAX_BANDS） */
    float preamp;                        /* 预增益线性系数（第一行，0~1） */
    eq_band_param_t bands[EQ_MAX_BANDS]; /* 各段参数（band_count 之后忽略） */
} eq_preset_t;

/** .eq 解析返回码（UI 据此区分 Toast 文案） */
enum {
    EQ_PARSE_OK = 0,      /* 解析成功 */
    EQ_PARSE_ERR_OPEN,    /* 文件打不开 */
    EQ_PARSE_ERR_TOO_MANY,/* 段数超过 EQ_MAX_BANDS */
    EQ_PARSE_ERR_INVALID, /* 内容无效（preamp/参数行格式错或超范围） */
    EQ_PARSE_ERR_EMPTY,   /* 空文件 / 缺少 preamp 或参数行 */
};

/** .eq 文件存放目录（SD 卡根目录 .musicfx，NightPlayer 兼容） */
#define EQ_MUSICFX_DIR "/sdcard/.musicfx"

/**
 * @brief 解析 .eq 文件（NightPlayer 兼容格式，见文件头注释）
 *
 * @param path 完整路径（如 "/sdcard/.musicfx/ZTYX.eq"）
 * @param out  输出预设；失败时 out 置全零（band_count=0），调用方可直接丢弃
 * @return EQ_PARSE_* 码
 */
int eq_parse_file(const char *path, eq_preset_t *out);

/**
 * @brief 按文件名解析 EQ（自动拼接 EQ_MUSICFX_DIR 目录）
 *
 * UI 弹窗用：name 为 eq_scan_files 返回的文件名（不含路径）。
 *
 * @param name .eq 文件名
 * @param out  输出预设（同 eq_parse_file）
 * @return EQ_PARSE_* 码
 */
int eq_parse_named(const char *name, eq_preset_t *out);

/**
 * @brief 扫描 /sdcard/.musicfx 下的 *.eq 文件（供 UI 弹窗列出选项）
 *
 * @param names 输出缓冲（每项 EQ_FILE_NAME_MAX 字节），目录缺失/为空返回 0
 * @param max   names 容量（最多列出 max 个）
 * @return 找到的文件数（≥0）
 */
int eq_scan_files(char (*names)[EQ_FILE_NAME_MAX], int max);

/* ------------------------------------------------------------------ */
/* DSP 运行时（audio 处理链使用）                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief 单块 biquad 运行时：归一化系数（a0 已除）+ 直接型 II 状态
 *
 * ⚠ 状态（x1/x2/y1/y2）只存在于 consumer 侧的持续快照里，跨处理块演化；
 * eq.c 内部模板（s_rt）的状态恒为 0，仅作为"参数 + 版本"的权威源。
 */
typedef struct {
    float b0, b1, b2;   /* 前馈系数 */
    float a1, a2;       /* 反馈系数 */
    float x1, x2;       /* 输入历史（参数版本变化时清零） */
    float y1, y2;       /* 输出历史（参数版本变化时清零） */
} eq_biquad_t;

/**
 * @brief EQ 运行时：band_count=0 表示 EQ 关闭（直通）
 *
 * ver：参数版本号。eq_apply / eq_notify_sample_rate / eq_reset_state 任一
 * 变更参数、系数或要求清状态时递增；eq_snapshot 检测到 consumer 侧版本
 * 落后时才整体重载（参数/系数 + 状态清零）。版本一致时快照零开销，
 * 滤波器状态在处理块间自然延续——绝不能在每块复制时覆盖状态，否则
 * 滤波器周期性"冷启动"会产生每块一个的瞬态爆音。
 */
typedef struct {
    uint8_t band_count;                  /* 有效段数（0 = 关闭） */
    float preamp;                        /* 预增益（线性） */
    uint32_t ver;                        /* 参数版本（见上） */
    eq_biquad_t bands[2][EQ_MAX_BANDS];  /* [声道 0=左 / 1=右][段] 级联链 */
} eq_runtime_t;

/**
 * @brief 应用 EQ 预设（任意线程可调，播放中切换也安全）
 *
 * 内部临界区整体替换运行时（系数在锁外按当前采样率预计算，锁内仅 memcpy）
 * 并清零滤波器状态（参数变了状态必须清，否则旧状态混入新系数产生瞬态）。
 *
 * @param preset 预设指针；NULL 或 band_count=0 表示关闭 EQ（直通）
 */
void eq_apply(const eq_preset_t *preset);

/**
 * @brief 采样率变化通知（audio_configure_fs 内调用）
 *
 * 滤波器系数与采样率相关：采样率变化时按当前激活参数重算系数并清状态。
 * 该调用点处于播放输出静止期（consumer 未启动），无样本处理并发。
 *
 * @param fs 新采样率（Hz；0 忽略）
 */
void eq_notify_sample_rate(uint32_t fs);

/** @brief 重置滤波器状态（切歌静止点调用：参数不变，仅清零历史） */
void eq_reset_state(void);

/**
 * @brief 取当前 EQ 运行时快照（audio_pcm_write 每块调用一次）
 *
 * ⚠ 快照只做"版本对齐"：consumer 侧状态（滤波器记忆）跨块延续，仅当
 * eq_apply/采样率变化/eq_reset_state 递增了版本（out->ver != 内部版本）
 * 才整体重载参数/系数并清零状态（播放中切换 EQ 从零状态起振，无旧状态
 * 混入新系数的瞬态）；版本一致时本函数零拷贝零开销。
 *
 * 首块调用前 out 应为全零（band_count=0 直通，ver=0 与内部一致）。
 *
 * @param out 输出快照（consumer 侧持续持有：处理期间状态在其中演化）
 */
void eq_snapshot(eq_runtime_t *out);

/**
 * @brief 单样本处理：x 归一化输入（int16/32768）→ ×预增益 → 级联各段
 *
 * EQ 关闭（band_count=0）时原样返回（不做任何处理）。
 * 返回值为处理后归一化值（可能超出 ±1，由上层乘回放增益后 Clamp）。
 * 内联以保证 -O2 下样本循环无函数调用开销。
 *
 * @param rt eq_snapshot 取到的快照
 * @param ch 声道（0=左，1=右）
 * @param x  输入样本（归一化）
 */
static inline float eq_biquad_process(eq_biquad_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static inline float eq_process_sample(eq_runtime_t *rt, int ch, float x)
{
    if (rt->band_count == 0) {
        return x;
    }
    x *= rt->preamp;
    eq_biquad_t *chain = rt->bands[ch];
    for (int i = 0; i < rt->band_count; i++) {
        x = eq_biquad_process(&chain[i], x);
    }
    return x;
}

#ifdef __cplusplus
}
#endif
