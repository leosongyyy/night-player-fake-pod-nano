#include "core/hardware/audio/eq.h"

#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eq";

/* ------------------------------------------------------------------ */
/* 激活状态（临界区保护：UI Core0 更新 vs audio Core1 快照读取）         */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static eq_preset_t  s_preset;   /* 当前激活预设参数（band_count=0 = 关闭） */
static uint32_t     s_fs = 0;   /* s_rt 系数对应的采样率（0 = 尚未配置） */
static eq_runtime_t s_rt;       /* 参数模板（"干净"快照）：系数 + 恒 0 状态；
                                   consumer 侧快照的滤波器状态在块间延续演化，
                                   不写回这里（见 eq_snapshot 版本机制） */

/* 编译期确认 eq_preset_t 无隐式 padding 差异（结构天然 4 字节对齐）：
   NVS blob 直存依赖其固定布局 */
_Static_assert(sizeof(eq_preset_t) == offsetof(eq_preset_t, bands) +
                                       EQ_MAX_BANDS * sizeof(eq_band_param_t),
               "eq_preset_t layout changed");

/* ------------------------------------------------------------------ */
/* RBJ Peaking EQ 系数计算（double 中间量，float 结果）                  */
/* ------------------------------------------------------------------ */

/* 一段 PeakEQ 的归一化系数（a0 已除），写入 b0/b1/b2/a1/a2。
   RBJ Audio EQ Cookbook：A = 10^(dB/40)，w0 = 2πfc/fs，alpha = sin(w0)/(2Q)：
     b0 = 1 + alpha*A, b1 = -2cos(w0), b2 = 1 - alpha*A
     a0 = 1 + alpha/A, a1 = -2cos(w0), a2 = 1 - alpha/A
   系数仅与采样率/参数相关，在 EQ 应用与采样率切换点计算（低频操作）。 */
static void eq_calc_peaking_coeffs(uint32_t fs, const eq_band_param_t *bp,
                                   eq_biquad_t *out)
{
    double fc = (double)bp->center;
    double q  = 1.0 / (double)bp->inv_q;
    double w0 = 2.0 * M_PI * fc / (double)fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double A = pow(10.0, (double)bp->gain_db / 40.0);
    double alpha = sw / (2.0 * q);

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cw;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha / A;

    out->b0 = (float)(b0 / a0);
    out->b1 = (float)(b1 / a0);
    out->b2 = (float)(b2 / a0);
    out->a1 = (float)(a1 / a0);
    out->a2 = (float)(a2 / a0);
    out->x1 = out->x2 = 0.0f;   /* 新系数必然伴随状态清零 */
    out->y1 = out->y2 = 0.0f;
}

/* 由预设参数 + 采样率构建完整运行时（状态全零；fs=0 或空预设 → 直通） */
static void eq_runtime_build(eq_runtime_t *rt, const eq_preset_t *p, uint32_t fs)
{
    memset(rt, 0, sizeof(*rt));
    if (p == NULL || p->band_count == 0 || fs == 0) {
        return;   /* band_count=0：直通（eq_process_sample 快速返回） */
    }
    uint8_t n = p->band_count;
    if (n > EQ_MAX_BANDS) {
        n = EQ_MAX_BANDS;
    }
    rt->band_count = n;
    rt->preamp = p->preamp;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < n; i++) {
            eq_calc_peaking_coeffs(fs, &p->bands[i], &rt->bands[ch][i]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* .eq 文件解析（NightPlayer 兼容）                                     */
/* ------------------------------------------------------------------ */

/* 解析失败时把多余字符吞掉（fgets 截断的超长行直接判无效，无需续读） */
static bool strip_line(char *line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != line) {
        memmove(line, p, strlen(p) + 1);
    }
    return line[0] != '\0';   /* false = 空行/空白行（调用方忽略） */
}

/* 单个数值 token 合法性：仅允许数字/小数点/正负号/科学计数指数（拦截
   "inf"/"nan"/字母/中文等），再用 strtod 完整消费校验（"1x" 之类残留
   字符会被 endptr 检出） */
static bool parse_float_token(const char *tok, float *out)
{
    if (*tok == '\0') {
        return false;
    }
    for (const char *p = tok; *p != '\0'; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
              c == 'e' || c == 'E')) {
            return false;
        }
    }
    char *end = NULL;
    float v = strtof(tok, &end);
    if (end == tok || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* preamp 行：整行必须是一个 0~1 的数值 token（"0.6" / "0.699842"） */
static bool parse_preamp_line(char *line, float *out)
{
    return parse_float_token(line, out);
}

/* 参数行："fCenter,fQ倒数,dBGain"，逗号恰好 2 个；三段各自必须是合法
   数值 token（行内其余字符仅允许空白，由 strip 后按逗号切分保证） */
static bool parse_band_line(char *line, eq_band_param_t *bp)
{
    /* 逗号计数必须正好 2 个（数字间用逗号分隔） */
    int commas = 0;
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == ',') {
            commas++;
        }
    }
    if (commas != 2) {
        return false;
    }
    char *tok1 = line;
    char *comma1 = strchr(line, ',');
    char *comma2 = strchr(comma1 + 1, ',');
    *comma1 = '\0';
    *comma2 = '\0';
    char *tok2 = comma1 + 1;
    char *tok3 = comma2 + 1;

    char *toks[3] = { tok1, tok2, tok3 };
    for (int i = 0; i < 3; i++) {
        strip_line(toks[i]);   /* trim 每段首尾空白 */
    }
    float fc = 0, invq = 0, db = 0;
    if (!parse_float_token(toks[0], &fc) ||
        !parse_float_token(toks[1], &invq) ||
        !parse_float_token(toks[2], &db)) {
        return false;
    }
    /* 参数范围校验（Q值在0.1-20之间，频率在16-20000之间，dB要在正负15范围内） */
    if (fc < 16.0f || fc > 20000.0f ||
        invq < 0.05f || invq > 10.0f ||
        db < -15.0f || db > 15.0f) {
        return false;
    }
    bp->center = fc;
    bp->inv_q = invq;
    bp->gain_db = db;
    return true;
}

int eq_parse_file(const char *path, eq_preset_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s failed", path);
        return EQ_PARSE_ERR_OPEN;
    }

    char line[128];   /* 单行最多 "数字,数字,数字" + 空白，128 绰绰有余 */
    bool have_preamp = false;
    float preamp = 0.0f;
    uint8_t bands = 0;
    int rc = EQ_PARSE_OK;

    while (fgets(line, sizeof(line), f) != NULL) {
        /* 超长行（读满缓冲未见换行）：格式非法，直接判无效 */
        if (strlen(line) == sizeof(line) - 1 && strchr(line, '\n') == NULL) {
            rc = EQ_PARSE_ERR_INVALID;
            break;
        }
        if (!strip_line(line)) {
            continue;   /* 空行/纯空白行忽略 */
        }

        if (!have_preamp) {
            /* 第一行：预增益（0~1 线性） */
            if (!parse_preamp_line(line, &preamp) ||
                preamp < 0.0f || preamp > 1.0f) {
                ESP_LOGW(TAG, "%s: invalid preamp line", path);
                rc = EQ_PARSE_ERR_INVALID;
                break;
            }
            have_preamp = true;
            continue;
        }

        /* 参数行：先判段数上限（超过 10 段单独报错，UI 弹对应提示） */
        if (bands >= EQ_MAX_BANDS) {
            ESP_LOGW(TAG, "%s: more than %d bands", path, (int)EQ_MAX_BANDS);
            rc = EQ_PARSE_ERR_TOO_MANY;
            break;
        }
        eq_band_param_t bp;
        if (!parse_band_line(line, &bp)) {
            ESP_LOGW(TAG, "%s: invalid band line '%s'", path, line);
            rc = EQ_PARSE_ERR_INVALID;
            break;
        }
        out->bands[bands++] = bp;
    }
    fclose(f);

    if (rc != EQ_PARSE_OK) {
        memset(out, 0, sizeof(*out));
        return rc;
    }
    if (!have_preamp || bands == 0) {
        /* 空文件 / 只有 preamp 没有参数段：无实用内容 */
        ESP_LOGW(TAG, "%s: empty (preamp=%d bands=%u)", path,
                 (int)have_preamp, (unsigned)bands);
        return EQ_PARSE_ERR_EMPTY;
    }

    out->band_count = bands;
    out->preamp = preamp;
    ESP_LOGI(TAG, "parsed %s: preamp=%.4f, %u bands", path, (double)preamp,
             (unsigned)bands);
    return EQ_PARSE_OK;
}

int eq_parse_named(const char *name, eq_preset_t *out)
{
    if (name == NULL || name[0] == '\0') {
        memset(out, 0, sizeof(*out));
        return EQ_PARSE_ERR_EMPTY;
    }
    char path[sizeof(EQ_MUSICFX_DIR) + EQ_FILE_NAME_MAX];
    snprintf(path, sizeof(path), "%s/%s", EQ_MUSICFX_DIR, name);
    return eq_parse_file(path, out);
}

/* ------------------------------------------------------------------ */
/* .eq 文件扫描（供 UI 弹窗列出可选预设）                                */
/* ------------------------------------------------------------------ */

/* 判断是否为 .eq 文件（扩展名大小写不敏感，排除 "." ".."） */
static bool is_eq_file(const char *name)
{
    if (name[0] == '\0') {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    return (strcasecmp(dot + 1, "eq") == 0);
}

/* 文件名比较（qsort：大小写不敏感字典序） */
static int cmp_eq_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int eq_scan_files(char (*names)[EQ_FILE_NAME_MAX], int max)
{
    if (names == NULL || max <= 0) {
        return 0;
    }
    DIR *dir = opendir(EQ_MUSICFX_DIR);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir %s failed", EQ_MUSICFX_DIR);
        return 0;   /* 无 .musicfx 目录 = 没有 EQ 文件 */
    }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && count < max) {
        const char *name = e->d_name;
        if (!is_eq_file(name) || strlen(name) >= EQ_FILE_NAME_MAX) {
            continue;
        }
        snprintf(names[count], EQ_FILE_NAME_MAX, "%s", name);
        count++;
    }
    closedir(dir);

    if (count > 1) {
        qsort(names, (size_t)count, EQ_FILE_NAME_MAX, cmp_eq_name);
    }
    ESP_LOGI(TAG, "scan %s: %d eq file(s)", EQ_MUSICFX_DIR, count);
    return count;
}

/* ------------------------------------------------------------------ */
/* 对外接口（临界区保护，见 eq.h 线程安全说明）                          */
/* ------------------------------------------------------------------ */

void eq_apply(const eq_preset_t *preset)
{
    /* 锁外按当前采样率预计算系数（含 pow/cos/sin，勿进临界区）；
       s_fs 为 uint32，临界区外读取原子 */
    uint32_t fs;
    taskENTER_CRITICAL(&s_mux);
    fs = s_fs;
    taskEXIT_CRITICAL(&s_mux);

    eq_runtime_t cand;
    eq_runtime_build(&cand, preset, fs);

    taskENTER_CRITICAL(&s_mux);
    if (preset != NULL && preset->band_count > 0) {
        s_preset = *preset;
    } else {
        memset(&s_preset, 0, sizeof(s_preset));
    }
    cand.ver = s_rt.ver + 1;   /* 版本递增：consumer 下个块整体重载并清状态 */
    s_rt = cand;
    taskEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "EQ %s (%u bands%s)", preset && preset->band_count > 0
             ? "enabled" : "disabled",
             preset ? (unsigned)preset->band_count : 0u,
             fs ? "": ", 系数待采样率配置");
}

void eq_notify_sample_rate(uint32_t fs)
{
    if (fs == 0) {
        return;
    }
    uint32_t old;
    taskENTER_CRITICAL(&s_mux);
    old = s_fs;
    taskEXIT_CRITICAL(&s_mux);
    if (fs == old) {
        return;   /* 采样率未变：系数仍有效，跳过重算 */
    }

    /* 锁外按当前参数重算系数（慢，勿进临界区；调用点处于输出静止期） */
    eq_preset_t p;
    eq_runtime_t cand;
    taskENTER_CRITICAL(&s_mux);
    p = s_preset;
    taskEXIT_CRITICAL(&s_mux);
    eq_runtime_build(&cand, &p, fs);

    taskENTER_CRITICAL(&s_mux);
    /* 极罕见：重算期间参数被 eq_apply 更新 → 以最新参数锁内补算一次 */
    if (memcmp(&p, &s_preset, sizeof(s_preset)) != 0) {
        eq_runtime_build(&cand, &s_preset, fs);
    }
    cand.ver = s_rt.ver + 1;
    s_rt = cand;
    s_fs = fs;
    taskEXIT_CRITICAL(&s_mux);
}

void eq_reset_state(void)
{
    taskENTER_CRITICAL(&s_mux);
    /* 模板状态保持全零 + 版本递增：consumer 检测到版本变化后，下一块
       从"零状态 + 相同参数"重载 → 滤波器记忆清零（切歌衔接），
       参数不变则系数无需重算 */
    if (s_rt.band_count > 0) {
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < s_rt.band_count; i++) {
                eq_biquad_t *b = &s_rt.bands[ch][i];
                b->x1 = b->x2 = 0.0f;
                b->y1 = b->y2 = 0.0f;
            }
        }
    }
    s_rt.ver++;
    taskEXIT_CRITICAL(&s_mux);
}

void eq_snapshot(eq_runtime_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    /* 版本一致 → 什么都不做：consumer 的状态（滤波器记忆）在块间延续，
       每块只多一次 4B 比较，零拷贝；
       版本落后（参数/系数/清状态请求）→ 整体重载模板：新参数/系数 + 状态清零 */
    if (out->ver != s_rt.ver) {
        *out = s_rt;
    }
    taskEXIT_CRITICAL(&s_mux);
}
