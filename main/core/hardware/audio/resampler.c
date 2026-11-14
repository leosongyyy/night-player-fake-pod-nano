#include "core/hardware/audio/resampler.h"

#include <math.h>
#include <string.h>

#include "dsps_dotprod.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "resampler";

/* 实例有效性标记：避免对未初始化/已释放的实例做 deinit 时 free 野指针
   （resampler_init 会先释放旧缓冲再重建；调用方实例可能未清零） */
#define RS_MAGIC 0x52534D50u

/* 从 PSRAM 分配（失败回退内部 RAM）：系数与窗口都只在播放线程访问，
 * 不参与 DMA，放 PSRAM 不影响实时性（点积会逐样本读窗口） */
static void *rs_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc %u B failed, fallback internal",
                 (unsigned)bytes);
        p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static int gcd_int(int a, int b)
{
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/**
 * 生成多相系数：
 *   输出位置 p = idx + frac/L（frac ∈ [0,L)），使用样本 idx-k（k=0..taps-1），
 *   距离 d = frac/L + k。窗口内 d 从 0 到 taps-1，中心 (taps-1)/2，
 *   低通截止 fc（相对输入奈奎斯特，下采样时收缩到 L/M×0.9 防混叠）。
 * 系数按"窗口里从旧到新"的顺序存放：j=0 对应 k=taps-1（最旧样本）。
 *
 * 性能：每相位只算一次 sinf/cosf 作为起点，之后按固定角步长做旋转递推
 * （sinc 角步长 π·fc，Blackman 角步长 2π/(taps-1)）。内层只有乘加和
 * 一次除法，避免 L×taps 次 sin/cos —— 早期双精度版本单次建系数约 5 s，
 * 会占满 Core1 触发 IDLE1 看门狗（真机日志实测）。
 */
static void build_coeffs(resampler_t *rs)
{
    const int L = rs->L;
    const int taps = rs->taps;
    const float center = (float)(taps - 1) * 0.5f;
    const float ratio = (float)rs->L / (float)rs->M;
    const float fc = (ratio < 1.0f) ? (ratio * 0.9f) : 0.9f;

    /* 角步长：j 每 +1，k 减 1 */
    const float ds = (float)M_PI * fc;
    const float dw = 2.0f * (float)M_PI / (float)(taps - 1);
    const float cs = cosf(ds), ss = sinf(ds);
    const float cw = cosf(dw), sw = sinf(dw);

    for (int ph = 0; ph < L; ph++) {
        const float frac = (float)ph / (float)L;
        const float d0 = frac + (float)(taps - 1);
        const float th0 = (float)M_PI * fc * (d0 - center);
        const float phi0 = 2.0f * (float)M_PI * d0 / (float)(taps - 1);

        float s = sinf(th0), c = cosf(th0);       /* sin/cos(sinc 角) */
        float cp = cosf(phi0), sp = sinf(phi0);   /* cos/sin(窗角) */

        float sum = 0.0f;
        float *coef = &rs->coef[(size_t)ph * taps];
        for (int j = 0; j < taps; j++) {
            const float theta = th0 - (float)j * ds;
            const float sinc = (fabsf(theta) < 1e-6f) ? 1.0f : (s / theta);
            /* cos(2φ) = 1 - 2 sin²φ，省一次 cos */
            const float w = 0.42f - 0.5f * cp + 0.08f * (1.0f - 2.0f * sp * sp);
            const float h = sinc * w;
            coef[j] = h;
            sum += h;

            /* 旋转到下一个 k（角度减去步长）
             * sin(θ-δ) = sinθ·cosδ - cosθ·sinδ
             * cos(θ-δ) = cosθ·cosδ + sinθ·sinδ */
            const float s_next = s * cs - c * ss;
            c = c * cs + s * ss;
            s = s_next;
            const float cp_next = cp * cw + sp * sw;
            sp = sp * cw - cp * sw;
            cp = cp_next;
        }
        /* 每相位归一化（DC 增益 1）：下采样时低通会掉增益，必须补回来 */
        if (fabsf(sum) > 1e-9f) {
            const float inv = 1.0f / sum;
            for (int j = 0; j < taps; j++) {
                coef[j] *= inv;
            }
        }
    }
}

esp_err_t resampler_init(resampler_t *rs, uint32_t fs_in, uint32_t fs_out,
                         int max_in_frames)
{
    if (rs == NULL || fs_in == 0 || fs_out == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_in_frames <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_in_frames > RESAMPLER_MAX_IN_FRAMES) {
        max_in_frames = RESAMPLER_MAX_IN_FRAMES;
    }

    /* 重复 init / 未初始化实例：只有 magic 有效时才释放旧缓冲 */
    if (rs->magic == RS_MAGIC) {
        resampler_deinit(rs);
    }
    memset(rs, 0, sizeof(*rs));
    rs->magic = RS_MAGIC;   /* 先置有效：后续分配失败时 deinit 能正确释放 */

    int g = gcd_int((int)fs_in, (int)fs_out);
    rs->fs_in  = fs_in;
    rs->fs_out = fs_out;
    rs->L = (int)(fs_out / (uint32_t)g);
    rs->M = (int)(fs_in / (uint32_t)g);
    rs->max_in = max_in_frames;
    rs->taps = RESAMPLER_TAPS;

    if (rs->L == 1 && rs->M == 1) {
        /* 直通：只留一个 1.0 系数，process 走拷贝分支 */
        rs->taps = 1;
        rs->coef = rs_alloc(sizeof(float));
        if (rs->coef == NULL) {
            return ESP_ERR_NO_MEM;
        }
        rs->coef[0] = 1.0f;
        rs->inited = true;
        rs->magic = RS_MAGIC;
        ESP_LOGI(TAG, "bypass %u Hz (no resampling)", (unsigned)fs_in);
        return ESP_OK;
    }

    const int hist = rs->taps - 1;
    rs->coef = rs_alloc((size_t)rs->L * rs->taps * sizeof(float));
    rs->win[0] = rs_alloc((size_t)(hist + rs->max_in) * sizeof(float));
    rs->win[1] = rs_alloc((size_t)(hist + rs->max_in) * sizeof(float));
    rs->hist[0] = rs_alloc((size_t)hist * sizeof(float));
    rs->hist[1] = rs_alloc((size_t)hist * sizeof(float));
    if (rs->coef == NULL || rs->win[0] == NULL || rs->win[1] == NULL ||
        rs->hist[0] == NULL || rs->hist[1] == NULL) {
        resampler_deinit(rs);
        return ESP_ERR_NO_MEM;
    }

    const int64_t t0 = esp_timer_get_time();
    build_coeffs(rs);
    const int64_t coeff_us = esp_timer_get_time() - t0;
    /* ⚠ 先置 inited 再 reset：resampler_reset() 会检查 inited，否则历史缓冲
       保持未初始化堆内存 → 第一批输出是 1e35 量级的噪声（2026-11 自测抓到） */
    rs->inited = true;
    resampler_reset(rs);
    rs->magic = RS_MAGIC;

    ESP_LOGI(TAG, "init %u -> %u Hz (L=%d M=%d taps=%d, %u B coeffs, %lld us)",
             (unsigned)fs_in, (unsigned)fs_out, rs->L, rs->M, rs->taps,
             (unsigned)((size_t)rs->L * rs->taps * sizeof(float)),
             (long long)coeff_us);
    return ESP_OK;
}

void resampler_deinit(resampler_t *rs)
{
    if (rs == NULL) {
        return;
    }
    if (rs->magic != RS_MAGIC) {
        /* 未初始化或已释放：不做 free，清零即可 */
        memset(rs, 0, sizeof(*rs));
        return;
    }
    heap_caps_free(rs->coef);
    heap_caps_free(rs->win[0]);
    heap_caps_free(rs->win[1]);
    heap_caps_free(rs->hist[0]);
    heap_caps_free(rs->hist[1]);
    rs->coef = NULL;
    rs->win[0] = rs->win[1] = NULL;
    rs->hist[0] = rs->hist[1] = NULL;
    rs->inited = false;
    rs->magic = 0;
}

void resampler_reset(resampler_t *rs)
{
    if (rs == NULL || !rs->inited) {
        return;
    }
    rs->pos_num = 0;
    rs->in_total = 0;
    if (rs->taps > 1) {
        memset(rs->hist[0], 0, (size_t)(rs->taps - 1) * sizeof(float));
        memset(rs->hist[1], 0, (size_t)(rs->taps - 1) * sizeof(float));
    }
}

size_t resampler_output_bound(const resampler_t *rs, size_t frames)
{
    if (rs == NULL || !rs->inited || rs->M <= 0) {
        return 0;
    }
    return (frames * (size_t)rs->L + (size_t)rs->M - 1) / (size_t)rs->M + 2;
}

size_t resampler_process(resampler_t *rs, const float *in, size_t frames,
                         float *out, size_t out_cap)
{
    if (rs == NULL || !rs->inited || in == NULL || out == NULL ||
        frames == 0 || out_cap == 0) {
        return 0;
    }
    if (frames > (size_t)rs->max_in) {
        frames = (size_t)rs->max_in;
    }

    /* 直通：原样拷贝 */
    if (rs->L == 1 && rs->M == 1) {
        size_t n = frames < out_cap ? frames : out_cap;
        memcpy(out, in, n * 2 * sizeof(float));
        return n;
    }

    const int taps = rs->taps;
    const int hist = taps - 1;

    /* 组装窗口：历史 + 本次输入（每声道一份连续数组，供点积使用） */
    for (int ch = 0; ch < 2; ch++) {
        float *w = rs->win[ch];
        memcpy(w, rs->hist[ch], (size_t)hist * sizeof(float));
        float *dst = w + hist;
        for (size_t i = 0; i < frames; i++) {
            dst[i] = in[2 * i + ch];
        }
    }

    const int64_t block_end = rs->in_total + (int64_t)frames;
    size_t nout = 0;
    while (nout < out_cap) {
        const int64_t p = rs->pos_num / rs->L;
        if (p >= block_end) {
            break;
        }
        const int64_t idx_rel = p - rs->in_total;
        if (idx_rel < 0) {
            /* 输出缓冲提前用完，且下一块的历史已覆盖不到这个位置：
               放弃本轮剩余输出（正常不会发生：out_cap 按上界分配） */
            break;
        }
        const int frac = (int)(rs->pos_num % rs->L);
        /* 窗口里样本 idx-k 的位置 = (idx_rel - k) + hist；
           系数数组按 j（旧→新）排列，从 (idx_rel - taps + 1 + hist) 开始 */
        const int pos = (int)idx_rel - taps + 1 + hist;
        const float *cp = &rs->coef[(size_t)frac * taps];
        for (int ch = 0; ch < 2; ch++) {
            float y = 0.0f;
            dsps_dotprod_f32(rs->win[ch] + pos, cp, &y, taps);
            out[2 * nout + ch] = y;
        }
        nout++;
        rs->pos_num += rs->M;
    }

    rs->in_total = block_end;

    /* 保存最后 hist 个输入样本作为下一次的历史（窗口总长 hist+frames，
       最后 hist 个样本从下标 frames 开始） */
    for (int ch = 0; ch < 2; ch++) {
        memcpy(rs->hist[ch], rs->win[ch] + frames,
               (size_t)hist * sizeof(float));
    }
    return nout;
}

/* ------------------------------------------------------------------ */
/* 自测（调试用，验证多相重采样正确性）                                  */
/* ------------------------------------------------------------------ */

void resampler_selftest(void)
{
    resampler_t rs = {0};
    /* 缓冲临时分配（本函数只在调试时调用；不占 BSS，避免吃掉内部 RAM）。
       ⚠ out_cap 的单位是**帧**：缓冲按 1024 帧分配，process 也只传 1024。 */
    float *in = rs_alloc(RESAMPLER_MAX_IN_FRAMES * 2 * sizeof(float));
    float *out = rs_alloc(1024 * 2 * sizeof(float));
    if (in == NULL || out == NULL ||
        resampler_init(&rs, 44100, 48000, RESAMPLER_MAX_IN_FRAMES) != ESP_OK) {
        ESP_LOGE(TAG, "selftest: init failed");
        heap_caps_free(in);
        heap_caps_free(out);
        return;
    }

    const double dphase = 2.0 * M_PI * 1000.0 / 44100.0;
    double phase = 0.0;
    double sum2 = 0.0;
    size_t total = 0;
    int zc = 0;
    float prev = 0.0f;
    const int blocks = 40;   /* 40×512 帧 ≈ 0.46s 输入 */

    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < RESAMPLER_MAX_IN_FRAMES; i++) {
            float v = (float)(0.5 * sin(phase));
            phase += dphase;
            in[2 * i] = v;
            in[2 * i + 1] = v;
        }
        size_t n = resampler_process(&rs, in, RESAMPLER_MAX_IN_FRAMES,
                                     out, 1024);
        for (size_t i = 0; i < n; i++) {
            float v = out[2 * i];
            sum2 += (double)v * v;
            if (prev <= 0.0f && v > 0.0f) {
                zc++;
            }
            prev = v;
        }
        total += n;
    }

    double rms = sqrt(sum2 / (double)(total > 0 ? total : 1));
    double secs = (double)total / 48000.0;
    double freq = (secs > 0.0) ? (zc / secs) : 0.0;
    resampler_deinit(&rs);
    heap_caps_free(in);
    heap_caps_free(out);
    ESP_LOGI(TAG, "selftest 44100->48000: in=%d frames, out=%u frames, "
                  "rms=%.3f (expect ~0.354), freq=%.1f Hz (expect 1000)",
             blocks * RESAMPLER_MAX_IN_FRAMES, (unsigned)total, rms, freq);
}
