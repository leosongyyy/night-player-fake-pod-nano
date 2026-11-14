/**
 * @file lvgl_ppa_rotate.c
 * 实验性：用 PPA 的 SRM（缩放-旋转-镜像）把 LVGL 画面顺时针旋转 0/90/180/270°
 *
 * 完整背景、几何推导与实测数据见 ideas_and_exps/PPA_ROTATE.md。
 *
 * 数据流（LVGL PARTIAL 渲染，双缓冲）：
 *
 *   LVGL 渲染一块 area(w×h) 到紧凑绘制缓冲 px_map
 *        │
 *        ├─ 角度 0（关闭）→ 原路径：就地 rgb565 swap → 按原坐标提交
 *        │
 *        └─ 角度 90/180/270 → PPA SRM 旋转到 s_tile（尺寸随角度互换或不变）
 *                 → rgb565 swap（面板要高字节在前）→ 按旋转后的坐标提交
 *
 * 为什么旋转输出只要**一块**缓冲：LVGL 9 的 `draw_buf_flush()` 在调用
 * flush_cb 之前会 `wait_for_flushing()`（见 lv_refr.c），即上一块缓冲的
 * QSPI 传输**已经完成**才会进入本次 flush。所以 flush_cb 执行期间没有 DMA
 * 在读 s_tile，单缓冲安全（也省下内部 DMA RAM——这里紧张）。
 *
 * 运行时切换：角度存在 config_store（NVS），启动时由 lvgl_port_init 传入；
 * 开发者选项里切换时调用 `lvgl_ppa_rotate_set_angle()` 并全屏重绘（见
 * lvgl_port_set_display_rotation）。旋转缓冲在开机时**总是**分配，所以切换
 * 不需要重新申请内部 RAM（开机末期碎片化，运行时申请很可能失败）。
 *
 * 触摸：本模块只负责画面。触摸坐标由 core/ui/lvgl_port.c 的 read_cb 按
 * **当前生效角度做逆变换**（画面顺时针转 θ，触摸就逆时针转 θ），再按
 * 左右手做 29/30 缩放校准——见那边 TOUCH_CAL_NUM 上方的说明。
 */

#include "core/ui/lvgl_ppa_rotate.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_UI_DISPLAY_ROTATE_PPA

#include "driver/ppa.h"

static const char *TAG = "ppa_rotate";

/* PPA 输出缓冲的对齐要求：内部 RAM 需按 L1 cache line 对齐（S31 = 64B），
   且 out.buffer_size 也要对齐（驱动会按对齐后的长度做 cache 回写/失效）。
   与 core/ui/lvgl_ppa_accel.c 用同一套常量，原因相同。 */
#define PPA_ROT_ALIGN       64
#define PPA_ROT_ALIGN_UP(x) (((x) + PPA_ROT_ALIGN - 1) & ~(size_t)(PPA_ROT_ALIGN - 1))

/* 角度索引：cw/90 → 0..4（0° / 90° / 180° / 270°）。0xFF 表示该角度不可用。 */
#define PPA_ROT_ANGLES      4
#define PPA_ROT_UNSUPPORTED 0xFF

static ppa_client_handle_t s_srm = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static uint8_t *s_tile = NULL;       /* 旋转输出（DMA，单块，见文件头注释） */
static size_t s_tile_size = 0;       /* 已按 64B 向上取整 */
static uint32_t s_w_res = 0;         /* 旋转前逻辑宽（LVGL 的 hor_res） */
static uint32_t s_h_res = 0;         /* 旋转前逻辑高（LVGL 的 ver_res，方形屏 = 宽） */
static uint16_t s_angle_cw = 0;       /* 当前生效的顺时针角度：0/90/180/270 */
static uint8_t s_ppa_angle[PPA_ROT_ANGLES] = {
    PPA_SRM_ROTATION_ANGLE_0, PPA_ROT_UNSUPPORTED,
    PPA_SRM_ROTATION_ANGLE_180, PPA_ROT_UNSUPPORTED,
};
static bool s_ready = false;
static uint32_t s_err_cnt = 0;

/* 一次性吞吐统计（CONFIG_UI_DISPLAY_ROTATE_STATS）：累计前 500 次旋转的
   耗时与像素数，打一条日志后停止计时。目的是给"PPA 旋转到底花多少时间"
   一个可信数字；关闭时这些变量与计时都不存在。 */
#if CONFIG_UI_DISPLAY_ROTATE_STATS
#define PPA_ROT_STATS_FLUSHES  500
static uint32_t s_flush_cnt = 0;
static int64_t  s_rot_us_sum = 0;
static uint64_t s_px_sum = 0;
#endif

/* 把 src 的 w×h 紧凑 RGB565 图旋转 90° 后写入 s_tile（h×w 紧凑布局）。
   角度由调用方给出（PPA 的"逆时针"约定，自检阶段已确认与 CW 的对应关系）。 */
static esp_err_t rotate_tile(const uint8_t *src, uint32_t w, uint32_t h, uint8_t ppa_angle)
{
    const bool swap_dims = (ppa_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                            ppa_angle == PPA_SRM_ROTATION_ANGLE_270);

    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = src,
            .pic_w = w,
            .pic_h = h,
            .block_w = w,
            .block_h = h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_tile,
            .buffer_size = (uint32_t)s_tile_size,
            /* 90/270 后输出图尺寸与输入互换（宽 = h、高 = w）；0/180 不变。
               步长随之是输出宽度×2，正好等于面板窗口宽度（紧凑行）。 */
            .pic_w = swap_dims ? h : w,
            .pic_h = swap_dims ? w : h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = (ppa_srm_rotation_angle_t)ppa_angle,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        /* 字节序交换（面板要的高字节在前）放在旋转之后由 CPU 做：
           PPA 的 byte_swap 作用于输入侧，转出来的仍是小端 RGB565；
           换到输出侧再 swap 一次，语义与原来的 flush 路径完全等价。 */
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_srm, &cfg);
}

/**
 * 方向自检：用一张 32x32 已知图案跑一次 PPA 旋转，按给定**顺时针角度**的
 * 映射逐像素比对：
 *      0°   : (x, y)
 *      90°  : (n-1-y, x)
 *      180° : (n-1-x, n-1-y)
 *      270° : (y, n-1-x)
 *
 * 为什么需要它：PPA 的 `ppa_srm_rotation_angle_t` 文档只说"逆时针"，
 * 而"逆时针"是相对图像坐标（y 向下）还是数学坐标并无明文；写反了画面
 * 就会变成另一个方向，只有上屏才能发现。启动时用 CPU 参考实现校验一遍，
 * 就能把"90° 用 270 还是 90"的对应关系确定下来（见 init）。
 */
#define ROT_SELFTEST_SIDE  32

static bool rotate_selftest(uint16_t cw_deg, uint8_t ppa_angle)
{
    const uint32_t n = ROT_SELFTEST_SIDE;
    uint16_t *src = heap_caps_aligned_alloc(PPA_ROT_ALIGN, n * n * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (src == NULL) {
        return false;
    }

    /* 每个像素一个唯一值（x、y 都在 0..31，拼成的 16 位值互不相同）。
       用 16 位"原始值"而不是真实颜色，是为了让比对不受任何颜色处理影响。 */
    for (uint32_t y = 0; y < n; y++) {
        for (uint32_t x = 0; x < n; x++) {
            src[y * n + x] = (uint16_t)((y << 8) | x);
        }
    }

    bool ok = (rotate_tile((const uint8_t *)src, n, n, ppa_angle) == ESP_OK);
    if (ok) {
        const uint16_t *out = (const uint16_t *)s_tile;
        for (uint32_t y = 0; y < n && ok; y++) {
            for (uint32_t x = 0; x < n; x++) {
                uint32_t dx, dy;
                switch (cw_deg) {
                case 90:  dx = n - 1 - y; dy = x;         break;
                case 180: dx = n - 1 - x; dy = n - 1 - y; break;
                case 270: dx = y;         dy = n - 1 - x; break;
                default:  dx = x;         dy = y;         break;   /* 0° */
                }
                if (out[dy * n + dx] != src[y * n + x]) {
                    ok = false;
                    break;
                }
            }
        }
    }

    heap_caps_free(src);
    return ok;
}

bool lvgl_ppa_rotate_init(esp_lcd_panel_handle_t panel_handle,
                          uint32_t hor_res, uint32_t ver_res,
                          size_t max_tile_bytes, uint16_t initial_cw_deg)
{
    if (s_ready) {
        return true;
    }
    if (panel_handle == NULL || max_tile_bytes == 0 || hor_res == 0 || ver_res == 0) {
        ESP_LOGE(TAG, "参数非法（panel=%p tile=%u %ux%u）", panel_handle,
                 (unsigned)max_tile_bytes, (unsigned)hor_res, (unsigned)ver_res);
        return false;
    }
    /* 本实验只处理方形屏：旋转后的目标区域仍落在原分辨率内（见 flush 里的
       坐标映射，目标 x 用 ver_res、y 用 hor_res，只有 H==W 才自洽）。 */
    if (hor_res != ver_res) {
        ESP_LOGE(TAG, "PPA 旋转实验仅支持方形屏（当前 %ux%u）", (unsigned)hor_res,
                 (unsigned)ver_res);
        return false;
    }

    s_tile_size = PPA_ROT_ALIGN_UP(max_tile_bytes);
    s_tile = heap_caps_aligned_alloc(PPA_ROT_ALIGN, s_tile_size, MALLOC_CAP_DMA);
    if (s_tile == NULL) {
        ESP_LOGE(TAG, "旋转输出缓冲分配失败（%u B，DMA 内 RAM）——画面保持未旋转",
                 (unsigned)s_tile_size);
        return false;
    }

    ppa_client_config_t client_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t err = ppa_register_client(&client_cfg, &s_srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 PPA SRM client 失败：%s", esp_err_to_name(err));
        heap_caps_free(s_tile);
        s_tile = NULL;
        return false;
    }

    s_panel = panel_handle;
    s_w_res = hor_res;
    s_h_res = ver_res;

    /* 确定"顺时针 90°"对应 PPA 的哪个角度：先按文档约定试 270° CCW，
       不对再试 90°。180° 在两个约定下相同，可直接用。 */
    if (rotate_selftest(90, PPA_SRM_ROTATION_ANGLE_270)) {
        s_ppa_angle[1] = PPA_SRM_ROTATION_ANGLE_270;
        s_ppa_angle[3] = PPA_SRM_ROTATION_ANGLE_90;
    } else if (rotate_selftest(90, PPA_SRM_ROTATION_ANGLE_90)) {
        ESP_LOGW(TAG, "PPA 270° 的旋转方向与预期不符，改按 90° 使用（硬件约定相反）");
        s_ppa_angle[1] = PPA_SRM_ROTATION_ANGLE_90;
        s_ppa_angle[3] = PPA_SRM_ROTATION_ANGLE_270;
    } else {
        ESP_LOGE(TAG, "PPA 旋转方向自检失败，画面保持未旋转");
        ppa_unregister_client(s_srm);
        s_srm = NULL;
        heap_caps_free(s_tile);
        s_tile = NULL;
        return false;
    }
    /* 顺带验证 180°/270° 的映射，避免"90° 对、其它角度错"的映射表问题 */
    if (!rotate_selftest(180, s_ppa_angle[2]) || !rotate_selftest(270, s_ppa_angle[3])) {
        ESP_LOGE(TAG, "PPA 180°/270° 自检失败，画面保持未旋转");
        ppa_unregister_client(s_srm);
        s_srm = NULL;
        heap_caps_free(s_tile);
        s_tile = NULL;
        return false;
    }

    s_ready = true;
    s_angle_cw = 0;
    if (!lvgl_ppa_rotate_set_angle(initial_cw_deg)) {
        ESP_LOGW(TAG, "初始角度 %u° 不可用，本次开机保持不旋转",
                 (unsigned)initial_cw_deg);
    }
    ESP_LOGI(TAG, "PPA 旋转已就绪：当前 %u°（顺时针），输出缓冲 %u B，触摸跟随旋转",
             (unsigned)s_angle_cw, (unsigned)s_tile_size);
    return true;
}

bool lvgl_ppa_rotate_set_angle(uint16_t cw_deg)
{
    if (!s_ready) {
        return false;
    }
    if (cw_deg != 0 && cw_deg != 90 && cw_deg != 180 && cw_deg != 270) {
        return false;
    }
    if (cw_deg != 0 && s_ppa_angle[cw_deg / 90] == PPA_ROT_UNSUPPORTED) {
        return false;
    }
    s_angle_cw = cw_deg;
    return true;
}

uint16_t lvgl_ppa_rotate_get_angle(void)
{
    return s_angle_cw;
}

bool lvgl_ppa_rotate_available(void)
{
    return s_ready;
}

bool lvgl_ppa_rotate_flush(const lv_area_t *area, uint8_t *px_map)
{
    if (!s_ready || s_angle_cw == 0 || area == NULL || px_map == NULL) {
        return false;
    }

    const int32_t w32 = lv_area_get_width(area);
    const int32_t h32 = lv_area_get_height(area);
    if (w32 <= 0 || h32 <= 0) {
        return false;
    }
    const uint32_t w = (uint32_t)w32;
    const uint32_t h = (uint32_t)h32;
    const size_t bytes = (size_t)w * h * sizeof(uint16_t);
    if (bytes > s_tile_size) {
        /* 理论上不会发生：LVGL 的单块面积由绘制缓冲大小决定，
           旋转前后像素数相同。真发生就回退（宁可这一块不转，也不能越界）。 */
        if (s_err_cnt++ < 3) {
            ESP_LOGW(TAG, "刷新块过大，回退未旋转路径（%ux%u，%u B > %u B）",
                     (unsigned)w, (unsigned)h, (unsigned)bytes, (unsigned)s_tile_size);
        }
        return false;
    }

#if CONFIG_UI_DISPLAY_ROTATE_STATS
    const int64_t t0 = (s_flush_cnt < PPA_ROT_STATS_FLUSHES) ? esp_timer_get_time() : 0;
#endif
    const esp_err_t err = rotate_tile(px_map, w, h, s_ppa_angle[s_angle_cw / 90]);
    if (err != ESP_OK) {
        if (s_err_cnt++ < 3) {
            ESP_LOGE(TAG, "PPA 旋转失败：%s（%ux%u，%u°），回退未旋转路径",
                     esp_err_to_name(err), (unsigned)w, (unsigned)h, (unsigned)s_angle_cw);
        }
        return false;
    }

    /* 面板期望"高字节在前"的 RGB565，与未旋转路径里的 lv_draw_sw_rgb565_swap
       完全等价；放在旋转之后做，是因为 PPA 只提供输入侧字节交换。 */
    lv_draw_sw_rgb565_swap(s_tile, w * h);

    /* 顺时针旋转的坐标映射（图像坐标，y 向下；屏为方形，W==H==460）：
           源 (x, y) → 目标 (W-1-y, x)        (90°)
                      → 目标 (W-1-x, H-1-y)    (180°)
                      → 目标 (y, H-1-x)        (270°)
       源区域 [x1..x2]×[y1..y2] 的目标矩形见下（端点包含）。
       面板驱动内部会再加 X gap，与未旋转路径一致。 */
    int32_t dx1, dy1, dx2, dy2;
    switch (s_angle_cw) {
    case 180:
        dx1 = (int32_t)s_w_res - 1 - area->x2;
        dy1 = (int32_t)s_h_res - 1 - area->y2;
        dx2 = (int32_t)s_w_res - 1 - area->x1;
        dy2 = (int32_t)s_h_res - 1 - area->y1;
        break;
    case 270:
        dx1 = area->y1;
        dy1 = (int32_t)s_w_res - 1 - area->x2;
        dx2 = area->y2;
        dy2 = (int32_t)s_w_res - 1 - area->x1;
        break;
    case 90:
    default:
        dx1 = (int32_t)s_h_res - 1 - area->y2;
        dy1 = area->x1;
        dx2 = (int32_t)s_h_res - 1 - area->y1;
        dy2 = area->x2;
        break;
    }

    const esp_err_t derr = esp_lcd_panel_draw_bitmap(s_panel, dx1, dy1, dx2 + 1, dy2 + 1, s_tile);
    if (derr != ESP_OK) {
        if (s_err_cnt++ < 3) {
            ESP_LOGE(TAG, "面板提交失败：%s（目标 %dx%d+%d+%d），回退未旋转路径",
                     esp_err_to_name(derr), (int)(dx2 - dx1 + 1), (int)(dy2 - dy1 + 1),
                     (int)dx1, (int)dy1);
        }
        return false;
    }

#if CONFIG_UI_DISPLAY_ROTATE_STATS
    if (s_flush_cnt < PPA_ROT_STATS_FLUSHES) {
        s_rot_us_sum += esp_timer_get_time() - t0;
        s_px_sum += (uint64_t)w * h;
        s_flush_cnt++;
        if (s_flush_cnt == PPA_ROT_STATS_FLUSHES) {
            /* 只打一次：平均耗时 + 吞吐（Mpx/s 的 10 倍，避免浮点） */
            const uint64_t mpx10 = (s_px_sum * 10u) / (uint64_t)(s_rot_us_sum > 0 ? s_rot_us_sum : 1);
            ESP_LOGI(TAG, "前 %d 次旋转：旋转路径平均 %lld us/次（含 PPA + swap + 提交），等效 %llu.%llu Mpx/s",
                     PPA_ROT_STATS_FLUSHES, (long long)(s_rot_us_sum / PPA_ROT_STATS_FLUSHES),
                     (unsigned long long)(mpx10 / 10), (unsigned long long)(mpx10 % 10));
        }
    }
#endif
    return true;
}

#else /* !CONFIG_UI_DISPLAY_ROTATE_PPA */

/* 未启用：调用点已被编译期裁掉，这里只留空实现防止误链接。 */
bool lvgl_ppa_rotate_init(esp_lcd_panel_handle_t panel_handle,
                          uint32_t hor_res, uint32_t ver_res,
                          size_t max_tile_bytes, uint16_t initial_cw_deg)
{
    (void)panel_handle;
    (void)hor_res;
    (void)ver_res;
    (void)max_tile_bytes;
    (void)initial_cw_deg;
    return false;
}

bool lvgl_ppa_rotate_set_angle(uint16_t cw_deg)
{
    (void)cw_deg;
    return false;
}

uint16_t lvgl_ppa_rotate_get_angle(void)
{
    return 0;
}

bool lvgl_ppa_rotate_available(void)
{
    return false;
}

bool lvgl_ppa_rotate_flush(const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    return false;
}

#endif /* CONFIG_UI_DISPLAY_ROTATE_PPA */
