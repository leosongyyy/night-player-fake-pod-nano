#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file resampler.h
 * @brief 立体声浮点多相重采样器（蓝牙 A2DP SBC 输出用）
 *
 * 背景：MP3 的采样率可能是 32k / 44.1k / 48k，而 A2DP SBC 协商出来的
 * 采样率是 48k 或 44.1k（本项目优先 48k）。两者不一致时必须重采样，
 * 否则耳机端会以错误速率播放（音调不对/卡顿）。
 *
 * 实现：**有理数多相窗函数 sinc 插值**：
 *   - 比值 fs_out/fs_in 约分得到 L/M（如 44.1k→48k 是 L=160/M=147）；
 *   - 预先算好 L 个相位、每相位 `taps` 个系数的低通 FIR（Blackman 窗，
 *     截止频率随下采样比例收缩到输出奈奎斯特的 0.9 倍，防混叠）；
 *   - 每个输出帧用一次 `dsps_dotprod_f32()`（esp-dsp，S31 上落到 ANSI C
 *     实现）算内积，所以这里确实**用到了 esp-dsp**，而不是手写点积；
 *   - 处理顺序：本模块只做"重采样"这一件事。**EQ 在重采样之后**按蓝牙
 *     输出采样率处理（见 bt_audio.c 的处理链），因此 EQ 系数与输出 fs
 *     保持一致，不会因为源采样率不同而偏频。
 *
 * 版本：系数/历史缓冲都从 PSRAM 分配（20KB 上下，不能占内部 RAM）；
 * 每声道一份连续窗口（历史 taps-1 帧 + 本次输入），点积要求输入连续。
 *
 * 线程模型：单个实例只由播放输出线程（mp3_out，Core1）调用；初始化/复位
 * 也在同一线程或输出静止期进行，无锁。
 */

/** 每相位滤波器抽头数（32：Blackman 窗下阻带约 -74dB，过渡带约 0.17×奈奎斯特） */
#define RESAMPLER_TAPS  32

/** 一次 process 允许的最大输入帧数（bt_audio 的块大小，512 帧 @48k ≈ 10.7ms） */
#define RESAMPLER_MAX_IN_FRAMES  512

/** 重采样器实例（内含 PSRAM 指针；用 resampler_init 创建、resampler_deinit 释放） */
typedef struct {
    uint32_t magic;      /* 内部：实例有效性标记（未初始化/已释放 = 0） */
    uint32_t fs_in;
    uint32_t fs_out;
    int      L;          /* 插值因子（fs_out / gcd） */
    int      M;          /* 抽取因子（fs_in / gcd） */
    int      taps;       /* 每相位抽头数 */
    int      max_in;     /* 单次最大输入帧数 */
    float   *coef;       /* L × taps 系数（PSRAM） */
    float   *win[2];     /* 每声道窗口：hist + max_in 帧（PSRAM） */
    float   *hist[2];    /* 每声道历史：taps-1 帧（PSRAM） */
    int64_t  pos_num;    /* 下一个输出样本的位置分子（单位 1/L 输入帧，绝对值） */
    int64_t  in_total;   /* 已送入的输入帧总数 */
    bool     inited;
} resampler_t;

/**
 * @brief 初始化重采样器（分配系数与窗口）
 *
 * @param rs           实例（调用方分配，通常内嵌在 bt_audio 的静态区）
 * @param fs_in        输入采样率（Hz）
 * @param fs_out       输出采样率（Hz）
 * @param max_in_frames 单次 process 的最大输入帧数（≤ RESAMPLER_MAX_IN_FRAMES）
 * @return ESP_OK 成功；参数非法/内存不足返回错误
 */
esp_err_t resampler_init(resampler_t *rs, uint32_t fs_in, uint32_t fs_out,
                         int max_in_frames);

/** @brief 释放内部缓冲（可重复调用） */
void resampler_deinit(resampler_t *rs);

/** @brief 清空历史与位置（切歌 / 切换输出 / 采样率变化后调用），保留系数 */
void resampler_reset(resampler_t *rs);

/**
 * @brief 处理一段交织立体声浮点样本
 *
 * @param rs    实例（已初始化）
 * @param in    [L0,R0,L1,R1,...]，in_frames 帧
 * @param frames 输入帧数（≤ max_in）
 * @param out   输出缓冲（交织立体声，容量 out_cap 帧）
 * @param out_cap 输出容量（帧）
 * @return 实际输出的帧数（可能为 0：输入不足一个输出周期）
 */
size_t resampler_process(resampler_t *rs, const float *in, size_t frames,
                         float *out, size_t out_cap);

/**
 * @brief 给定输入帧数时，输出帧数的上界（用于分配输出缓冲）
 *
 * = ceil(frames × L / M) + 2（+2 是相位余量）
 */
size_t resampler_output_bound(const resampler_t *rs, size_t frames);

/** @brief 当前实例是否直通（输入采样率 == 输出采样率） */
static inline bool resampler_is_bypass(const resampler_t *rs)
{
    return rs != NULL && rs->inited && rs->L == 1 && rs->M == 1;
}

/**
 * @brief 自测（调试用）：44100→48000 注入 1kHz 正弦，校验输出帧数/RMS/频率
 *
 * 不依赖蓝牙硬件，用于验证多相系数/索引与 esp-dsp 点积链路是否正确。
 * 结果打印到串口；无返回值。
 */
void resampler_selftest(void);

#ifdef __cplusplus
}
#endif
