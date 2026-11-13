/**
 * @file lvgl_ppa_accel.c
 * LVGL 9.4 自定义 SW blend handler —— 用 ESP32-S31 的 PPA 做硬件混合
 *
 * ─────────────────────────────────────────────────────────────────────────
 * 为什么不用 LVGL 自带的 PPA draw unit（CONFIG_LV_USE_PPA）
 * ─────────────────────────────────────────────────────────────────────────
 * 9.4 上游的 lv_draw_ppa*.c 是"独立绘制单元"，问题有三：
 *   1) FILL 把目标块固定画在 buffer 左上角，忽略 target_layer->buf_area，
 *      本项目是 PARTIAL 渲染（缓冲 460x46），表现为每 46 行一条横黑线；
 *   2) 非阻塞模式却立刻把 task 标记完成，刷屏可能早于 PPA 写完；
 *   3) 每次操作都对整块缓冲做 cache 同步，局部刷新下开销被放大。
 * 官方固件的日志（"PPA: register lvgl blend handle ok"）说明它走的是
 * 自定义 blend handler，与 OtoClock 用的 esp_lvgl_adapter 同一条路。
 *
 * ─────────────────────────────────────────────────────────────────────────
 * 这条路径做什么
 * ─────────────────────────────────────────────────────────────────────────
 * lv_draw_sw_register_blend_handler() 注册后，LVGL 把所有"向 RGB565 图层
 * 混合"的操作都交给本回调（lv_draw_sw_blend.c 的 handler 分支）。
 * 回调里按面积/格式/对齐分流：
 *   · RGB565 源图 + 无复杂 mask + 面积够大  → PPA blend（阻塞）
 *   · 纯色填充（CONFIG_UI_PPA_FILL_MIN_PX） → PPA fill（默认关闭，实测偏慢）
 *   · 其余（A8 文字、mask、小面积、其它格式）→ 回退 CPU 原路径
 *
 * ⚠ 回退路径必须与 lv_draw_sw_blend.c 的默认实现逐行一致（含与 src_area /
 *   mask_area 的二次求交），否则会出现"文字/图标位置偏移"这类隐蔽错误。
 *
 * ⚠ 输出缓冲限定在**内部 RAM**：PPA 要求 out.buffer 与 buffer_size 都按
 *   cache line 对齐，而 PSRAM 图层的 data_size 无法保证 64B 整数倍，
 *   驱动内部 msync 会越界几十字节。本项目的主绘制缓冲本来就是内部 DMA 内存，
 *   临时图层走 CPU 即可。
 */
#include "core/ui/lvgl_ppa_accel.h"

#include "sdkconfig.h"

#if CONFIG_UI_PPA_ACCEL

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"

#include "driver/ppa.h"
#include "soc/soc_caps.h"

/* LVGL 私有绘制接口：blend 描述符结构、面积求交与 rgb565 混合函数 */
#include "src/misc/lv_area_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.h"

static const char *TAG = "ppa_accel";

#define PPA_ALIGN_SIZE  64
#define PPA_ALIGN_UP(x) (((x) + PPA_ALIGN_SIZE - 1) & ~(size_t)(PPA_ALIGN_SIZE - 1))

static ppa_client_handle_t s_blend_handle = NULL;
#if CONFIG_UI_PPA_FILL_MIN_PX > 0
static ppa_client_handle_t s_fill_handle = NULL;
#endif
static bool s_ready = false;

#if CONFIG_UI_PPA_STATS
/* 混合流量画像：每 500 次混合打印一次各类别的调用数与 PPA 命中/耗时。
   用来回答"这套 UI 到底会不会走到 PPA"——如果 565半透明 一直是 0，
   说明 PPA 混合在这个界面上根本没有用武之地（文字是 A8，填充是 CPU）。 */
static uint32_t s_stat_calls = 0;
static uint32_t s_stat_fill = 0;
static uint32_t s_stat_masked = 0;
static uint32_t s_stat_a8 = 0;
static uint32_t s_stat_565_opaque = 0;
static uint32_t s_stat_565_alpha = 0;
static uint32_t s_stat_other = 0;
static uint32_t s_stat_ppa = 0;
static int64_t  s_stat_ppa_us = 0;
static uint64_t s_stat_mask_px = 0;
static uint64_t s_stat_fill_px = 0;
static uint64_t s_stat_565_opaque_px = 0;

static void ppa_stats_tick(void)
{
    s_stat_calls++;
    if((s_stat_calls % 500u) != 0u) {
        return;
    }
    ESP_LOGI(TAG, "blend x%u：fill=%u(avg %llu px) mask=%u(avg %llu px) A8=%u 565不透明=%u(avg %llu px) 565半透明=%u 其它=%u | PPA %u 次，平均 %lld us",
             (unsigned)s_stat_calls,
             (unsigned)s_stat_fill, (unsigned long long)(s_stat_fill ? s_stat_fill_px / s_stat_fill : 0),
             (unsigned)s_stat_masked, (unsigned long long)(s_stat_masked ? s_stat_mask_px / s_stat_masked : 0),
             (unsigned)s_stat_a8,
             (unsigned)s_stat_565_opaque,
             (unsigned long long)(s_stat_565_opaque ? s_stat_565_opaque_px / s_stat_565_opaque : 0),
             (unsigned)s_stat_565_alpha, (unsigned)s_stat_other,
             (unsigned)s_stat_ppa,
             (long long)(s_stat_ppa > 0 ? s_stat_ppa_us / s_stat_ppa : 0));
    s_stat_calls = 0;
    s_stat_fill = 0;
    s_stat_masked = 0;
    s_stat_a8 = 0;
    s_stat_565_opaque = 0;
    s_stat_565_alpha = 0;
    s_stat_other = 0;
    s_stat_ppa = 0;
    s_stat_ppa_us = 0;
    s_stat_mask_px = 0;
    s_stat_fill_px = 0;
    s_stat_565_opaque_px = 0;
}
#endif

static void ppa_sw_fallback(lv_draw_task_t *t, const lv_draw_sw_blend_dsc_t *dsc);

/* ───────────────────────── PPA 操作封装 ───────────────────────── */

static esp_err_t ppa_blend_rgb565(uint8_t *dst, const lv_area_t *dst_area,
                                  const void *src, const lv_area_t *src_area,
                                  uint32_t src_stride_px, const lv_area_t *block,
                                  lv_opa_t opa)
{
    uint32_t dst_w = (uint32_t)lv_area_get_width(dst_area);
    uint32_t dst_h = (uint32_t)lv_area_get_height(dst_area);
    uint32_t block_w = (uint32_t)lv_area_get_width(block);
    uint32_t block_h = (uint32_t)lv_area_get_height(block);

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = dst,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = (uint32_t)(block->x1 - dst_area->x1),
            .block_offset_y = (uint32_t)(block->y1 - dst_area->y1),
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = src,
            .pic_w = src_stride_px,
            .pic_h = (uint32_t)lv_area_get_height(src_area),
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = (uint32_t)(block->x1 - src_area->x1),
            .block_offset_y = (uint32_t)(block->y1 - src_area->y1),
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = (uint32_t)PPA_ALIGN_UP((size_t)dst_w * dst_h * 2),
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = (uint32_t)(block->x1 - dst_area->x1),
            .block_offset_y = (uint32_t)(block->y1 - dst_area->y1),
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        /* out = bg * (255-opa) + fg * opa，等价于 CPU 的"按 opa 叠加" */
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = (uint32_t)(255 - opa),
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = opa,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_blend(s_blend_handle, &cfg);
}

#if CONFIG_UI_PPA_FILL_MIN_PX > 0
static esp_err_t ppa_fill_rgb565(uint8_t *dst, const lv_area_t *dst_area,
                                 const lv_area_t *block, lv_color_t color)
{
    uint32_t dst_w = (uint32_t)lv_area_get_width(dst_area);
    uint32_t dst_h = (uint32_t)lv_area_get_height(dst_area);

    lv_color32_t c32 = lv_color_to_32(color, LV_OPA_COVER);
    uint32_t argb = ((uint32_t)c32.alpha << 24) | ((uint32_t)c32.red << 16) |
                    ((uint32_t)c32.green << 8) | (uint32_t)c32.blue;

    ppa_fill_oper_config_t cfg = {
        .out = {
            .buffer = dst,
            .buffer_size = (uint32_t)PPA_ALIGN_UP((size_t)dst_w * dst_h * 2),
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = (uint32_t)(block->x1 - dst_area->x1),
            .block_offset_y = (uint32_t)(block->y1 - dst_area->y1),
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = (uint32_t)lv_area_get_width(block),
        .fill_block_h = (uint32_t)lv_area_get_height(block),
        .fill_argb_color.val = argb,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_fill(s_fill_handle, &cfg);
}
#endif /* CONFIG_UI_PPA_FILL_MIN_PX > 0 */

/* ───────────────────────── CPU 回退 ─────────────────────────
 * 与 lv_draw_sw_blend.c 的默认实现保持一致：
 *   · 填充：只与 clip_area 求交
 *   · 图像：还要与 src_area / mask_area 求交；src_buf 按求交后的左上角偏移
 */
static void ppa_sw_fallback(lv_draw_task_t *t, const lv_draw_sw_blend_dsc_t *dsc)
{
    lv_layer_t *layer = t->target_layer;
    if(layer == NULL || layer->draw_buf == NULL) {
        return;
    }

    lv_area_t blend_area;
    if(!lv_area_intersect(&blend_area, dsc->blend_area, &t->clip_area)) {
        return;
    }

    uint32_t layer_stride = layer->draw_buf->header.stride;

    if(dsc->src_buf == NULL) {
        lv_draw_sw_blend_fill_dsc_t fill_dsc;
        lv_memzero(&fill_dsc, sizeof(fill_dsc));
        fill_dsc.dest_w = lv_area_get_width(&blend_area);
        fill_dsc.dest_h = lv_area_get_height(&blend_area);
        fill_dsc.dest_stride = (int32_t)layer_stride;
        fill_dsc.opa = dsc->opa;
        fill_dsc.color = dsc->color;
        fill_dsc.mask_stride = 0;

        if(dsc->mask_buf == NULL || dsc->mask_res == LV_DRAW_SW_MASK_RES_FULL_COVER) {
            fill_dsc.mask_buf = NULL;
        }
        else {
            fill_dsc.mask_buf = dsc->mask_buf;
        }

        fill_dsc.relative_area = blend_area;
        lv_area_move(&fill_dsc.relative_area, -layer->buf_area.x1, -layer->buf_area.y1);
        fill_dsc.dest_buf = lv_draw_layer_go_to_xy(layer,
                                                   blend_area.x1 - layer->buf_area.x1,
                                                   blend_area.y1 - layer->buf_area.y1);

        if(fill_dsc.mask_buf != NULL) {
            fill_dsc.mask_stride = dsc->mask_stride == 0
                                   ? lv_area_get_width(dsc->mask_area)
                                   : dsc->mask_stride;
            fill_dsc.mask_buf += fill_dsc.mask_stride * (blend_area.y1 - dsc->mask_area->y1)
                                 + (blend_area.x1 - dsc->mask_area->x1);
        }

        lv_draw_sw_blend_color_to_rgb565(&fill_dsc);
        return;
    }

    if(!lv_area_intersect(&blend_area, &blend_area, dsc->src_area)) {
        return;
    }
    if(dsc->mask_area != NULL && !lv_area_intersect(&blend_area, &blend_area, dsc->mask_area)) {
        return;
    }

    lv_draw_sw_blend_image_dsc_t image_dsc;
    lv_memzero(&image_dsc, sizeof(image_dsc));
    image_dsc.dest_w = lv_area_get_width(&blend_area);
    image_dsc.dest_h = lv_area_get_height(&blend_area);
    image_dsc.dest_stride = (int32_t)layer_stride;
    image_dsc.opa = dsc->opa;
    image_dsc.blend_mode = dsc->blend_mode;
    image_dsc.src_stride = (int32_t)dsc->src_stride;
    image_dsc.src_color_format = dsc->src_color_format;

    /* 与默认路径一致：v9 的 bpp 是"位"，要 >>3 得到每像素字节数 */
    uint32_t src_px_size = lv_color_format_get_bpp(dsc->src_color_format);
    src_px_size = (src_px_size + 7) >> 3;

    const uint8_t *src_buf = (const uint8_t *)dsc->src_buf;
    src_buf += (size_t)image_dsc.src_stride * (blend_area.y1 - dsc->src_area->y1);
    src_buf += (size_t)(blend_area.x1 - dsc->src_area->x1) * src_px_size;
    image_dsc.src_buf = src_buf;
    image_dsc.mask_stride = 0;

    if(dsc->mask_buf == NULL || dsc->mask_res == LV_DRAW_SW_MASK_RES_FULL_COVER) {
        image_dsc.mask_buf = NULL;
    }
    else {
        image_dsc.mask_buf = dsc->mask_buf;
    }

    if(image_dsc.mask_buf != NULL) {
        image_dsc.mask_stride = dsc->mask_stride != 0
                                ? dsc->mask_stride
                                : lv_area_get_width(dsc->mask_area);
        image_dsc.mask_buf += image_dsc.mask_stride * (blend_area.y1 - dsc->mask_area->y1)
                              + (blend_area.x1 - dsc->mask_area->x1);
    }

    image_dsc.relative_area = blend_area;
    lv_area_move(&image_dsc.relative_area, -layer->buf_area.x1, -layer->buf_area.y1);

    image_dsc.src_area = *dsc->src_area;
    lv_area_move(&image_dsc.src_area, -layer->buf_area.x1, -layer->buf_area.y1);

    image_dsc.dest_buf = lv_draw_layer_go_to_xy(layer,
                                                blend_area.x1 - layer->buf_area.x1,
                                                blend_area.y1 - layer->buf_area.y1);

    lv_draw_sw_blend_image_to_rgb565(&image_dsc);
}

/* ───────────────────────── blend handler ───────────────────────── */

static void ppa_blend_handler(lv_draw_task_t *t, const lv_draw_sw_blend_dsc_t *dsc)
{
    lv_layer_t *layer = t->target_layer;

#if CONFIG_UI_PPA_STATS
    ppa_stats_tick();
#endif

    if(!s_ready || layer == NULL || layer->draw_buf == NULL ||
       layer->color_format != LV_COLOR_FORMAT_RGB565 || dsc->blend_mode != LV_BLEND_MODE_NORMAL) {
        ppa_sw_fallback(t, dsc);
        return;
    }

    lv_area_t area;
    if(!lv_area_intersect(&area, dsc->blend_area, &t->clip_area)) {
        return;
    }

    /* 复杂 mask（圆角、渐变、字形以外的遮罩）PPA 不支持，直接回退 */
    if(dsc->mask_buf != NULL && dsc->mask_res != LV_DRAW_SW_MASK_RES_FULL_COVER &&
       dsc->mask_res != LV_DRAW_SW_MASK_RES_UNKNOWN) {
#if CONFIG_UI_PPA_STATS
        s_stat_masked++;
        s_stat_mask_px += lv_area_get_size(&area);
#endif
        ppa_sw_fallback(t, dsc);
        return;
    }

    const lv_area_t *buf_area = &layer->buf_area;
    uint8_t *dst = (uint8_t *)layer->draw_buf->data;
    int32_t buf_w = lv_area_get_width(buf_area);

    if(dst == NULL || !esp_ptr_internal(dst) ||
       ((uintptr_t)dst & (PPA_ALIGN_SIZE - 1)) != 0 ||
       layer->draw_buf->header.stride != (uint32_t)buf_w * 2) {
        ppa_sw_fallback(t, dsc);
        return;
    }

    if(area.x1 < buf_area->x1 || area.y1 < buf_area->y1 ||
       area.x2 > buf_area->x2 || area.y2 > buf_area->y2) {
        ppa_sw_fallback(t, dsc);
        return;
    }

    uint32_t area_px = lv_area_get_size(&area);

    /* ── 纯色填充（默认关闭：实测 PPA FILL 比 CPU 慢，见 PPA_ACCEL.md） ── */
    if(dsc->src_buf == NULL) {
#if CONFIG_UI_PPA_STATS
        s_stat_fill++;
        s_stat_fill_px += area_px;
#endif
#if CONFIG_UI_PPA_FILL_MIN_PX > 0
        if(dsc->opa >= LV_OPA_MAX && area_px >= (uint32_t)CONFIG_UI_PPA_FILL_MIN_PX) {
            if(ppa_fill_rgb565(dst, buf_area, &area, dsc->color) == ESP_OK) {
                return;
            }
        }
#endif
        ppa_sw_fallback(t, dsc);
        return;
    }

#if CONFIG_UI_PPA_STATS
    if(dsc->src_color_format == LV_COLOR_FORMAT_A8) {
        s_stat_a8++;
    }
    else if(dsc->src_color_format == LV_COLOR_FORMAT_RGB565) {
        if(dsc->opa >= LV_OPA_MAX) {
            s_stat_565_opaque++;
            s_stat_565_opaque_px += area_px;
        }
        else {
            s_stat_565_alpha++;
        }
    }
    else {
        s_stat_other++;
        /* 限流打印一次未知格式：切歌后封面若从 RGB565 变成别的格式，
           这里能直接看出来（面积也一并打印）。 */
        static int64_t s_last_other_log_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_other_log_us > 1000000) {
            s_last_other_log_us = now;
            ESP_LOGW(TAG, "其它格式混合：cf=%d area=%ldx%ld opa=%u mask=%d",
                     (int)dsc->src_color_format,
                     (long)lv_area_get_width(&area), (long)lv_area_get_height(&area),
                     (unsigned)dsc->opa, dsc->mask_buf != NULL);
        }
    }
#endif

    /* ── 图像混合：只接管 RGB565 源图 ── */
    if(dsc->src_color_format != LV_COLOR_FORMAT_RGB565 || dsc->src_area == NULL ||
       area_px < (uint32_t)CONFIG_UI_PPA_BLEND_MIN_PX) {
        ppa_sw_fallback(t, dsc);
        return;
    }

    /* 不透明图像在 CPU 路径上是整行 memcpy（实测 91us/21160px，PPA 要 672us），
       交给 PPA 反而慢 7 倍，必须留在 CPU。PPA 只在真正需要逐像素 alpha 混合时
       才有优势（实测半透明 21160px：CPU 1132us vs PPA 667us）。 */
    if(dsc->opa >= LV_OPA_MAX) {
        ppa_sw_fallback(t, dsc);
        return;
    }

    const lv_area_t *src_area = dsc->src_area;
    if(!lv_area_intersect(&area, &area, src_area)) {
        return;
    }
    if(dsc->mask_area != NULL && !lv_area_intersect(&area, &area, dsc->mask_area)) {
        return;
    }

    uint32_t src_stride = dsc->src_stride != 0
                          ? dsc->src_stride
                          : (uint32_t)lv_area_get_width(src_area) * 2;
    if((src_stride & 1u) != 0) {
        ppa_sw_fallback(t, dsc);
        return;
    }
    int32_t src_stride_px = (int32_t)(src_stride / 2);
    int32_t off_x = area.x1 - src_area->x1;
    int32_t off_y = area.y1 - src_area->y1;

    if(off_x < 0 || off_y < 0 ||
       off_x + lv_area_get_width(&area) > src_stride_px ||
       off_y + lv_area_get_height(&area) > lv_area_get_height(src_area)) {
        ppa_sw_fallback(t, dsc);
        return;
    }

#if CONFIG_UI_PPA_STATS
    int64_t t0 = esp_timer_get_time();
#endif
    esp_err_t err = ppa_blend_rgb565(dst, buf_area, dsc->src_buf, src_area,
                                     (uint32_t)src_stride_px, &area, dsc->opa);
#if CONFIG_UI_PPA_STATS
    if(err == ESP_OK) {
        s_stat_ppa++;
        s_stat_ppa_us += esp_timer_get_time() - t0;
    }
#endif

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_blend 失败：%s，回退 CPU", esp_err_to_name(err));
        ppa_sw_fallback(t, dsc);
    }
}

/* ───────────────────────── 初始化 ───────────────────────── */

void lvgl_ppa_accel_init(lv_display_t *disp)
{
    if(disp == NULL) {
        return;
    }
    if(lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        ESP_LOGW(TAG, "显示色彩格式不是 RGB565，PPA 混合加速不启用");
        return;
    }
    if(s_ready) {
        return;
    }

    /* cache line 大小决定驱动内部 msync 的粒度：内部 RAM 为 0/64 都正常，
       0 表示内部内存不经过缓存（这时 PPA 反而更快）。仅用于日志。 */
    uint8_t int_probe = 0;
    size_t int_align = esp_cache_get_line_size_by_addr(&int_probe);
    void *psram_probe = heap_caps_malloc(1, MALLOC_CAP_SPIRAM);
    size_t psram_align = esp_cache_get_line_size_by_addr(psram_probe);
    heap_caps_free(psram_probe);
    ESP_LOGI(TAG, "cache line：内部 %u B / PSRAM %u B", (unsigned)int_align, (unsigned)psram_align);

    ppa_client_config_t blend_cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t err = ppa_register_client(&blend_cfg, &s_blend_handle);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "注册 PPA blend client 失败：%s", esp_err_to_name(err));
        return;
    }

#if CONFIG_UI_PPA_FILL_MIN_PX > 0
    ppa_client_config_t fill_cfg = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    err = ppa_register_client(&fill_cfg, &s_fill_handle);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "注册 PPA fill client 失败：%s", esp_err_to_name(err));
        ppa_unregister_client(s_blend_handle);
        s_blend_handle = NULL;
        return;
    }
#endif

    static lv_draw_sw_custom_blend_handler_t handler = {
        .dest_cf = LV_COLOR_FORMAT_RGB565,
        .handler = ppa_blend_handler,
    };
    if(!lv_draw_sw_register_blend_handler(&handler)) {
        ESP_LOGE(TAG, "注册 LVGL blend handler 失败");
        ppa_unregister_client(s_blend_handle);
        s_blend_handle = NULL;
#if CONFIG_UI_PPA_FILL_MIN_PX > 0
        ppa_unregister_client(s_fill_handle);
        s_fill_handle = NULL;
#endif
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "PPA blend handler 已注册（面积门槛 %d px，fill 门槛 %d px）",
             CONFIG_UI_PPA_BLEND_MIN_PX, CONFIG_UI_PPA_FILL_MIN_PX);
}

/* ───────────────────────── 性能自测 ───────────────────────── */

#if CONFIG_UI_PPA_BENCH
#define BENCH_W 460
#define BENCH_H 46
#define BENCH_N 100

static uint8_t *bench_alloc(size_t bytes, uint32_t caps)
{
    return (uint8_t *)heap_caps_aligned_alloc(PPA_ALIGN_SIZE, PPA_ALIGN_UP(bytes),
                                              caps | MALLOC_CAP_8BIT);
}

/* 测一组 w x h 的半透明（opa=128）图像混合：CPU vs PPA。
   dst 固定按 460x46 的整块缓冲描述，只混左上角 w x h 的子块，
   与真实局部刷新时"一个绘制块里只有一小块在混合"的情形一致。 */
static void bench_alpha(int w, int h, uint8_t *dst, uint8_t *src)
{
    lv_area_t block = { .x1 = 0, .y1 = 0, .x2 = w - 1, .y2 = h - 1 };
    lv_area_t dst_area = { .x1 = 0, .y1 = 0, .x2 = BENCH_W - 1, .y2 = BENCH_H - 1 };
    lv_area_t src_area = dst_area;

    int64_t t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        lv_draw_sw_blend_image_dsc_t d;
        lv_memzero(&d, sizeof(d));
        d.dest_buf = dst;
        d.dest_w = w;
        d.dest_h = h;
        d.dest_stride = w * 2;
        d.src_buf = src;
        d.src_stride = BENCH_W * 2;
        d.src_color_format = LV_COLOR_FORMAT_RGB565;
        d.opa = LV_OPA_50;
        d.blend_mode = LV_BLEND_MODE_NORMAL;
        d.relative_area = block;
        d.src_area = block;
        lv_draw_sw_blend_image_to_rgb565(&d);
    }
    int64_t cpu = (esp_timer_get_time() - t0) / BENCH_N;

    t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        ppa_blend_rgb565(dst, &dst_area, src, &src_area, BENCH_W, &block, LV_OPA_50);
    }
    int64_t ppa = (esp_timer_get_time() - t0) / BENCH_N;

    ESP_LOGI(TAG, "alpha %3dx%-2d (%5d px): CPU %4lld us | PPA %4lld us",
             w, h, w * h, (long long)cpu, (long long)ppa);
}

void lvgl_ppa_accel_benchmark(void)
{
    if(!s_ready) {
        return;
    }

    const size_t bytes = (size_t)BENCH_W * BENCH_H * 2;
    /* dst 用内部 RAM（与真实绘制缓冲一致）；src 用 PSRAM（与封面图一致） */
    uint8_t *dst = bench_alloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *src = bench_alloc(bytes, MALLOC_CAP_SPIRAM);
    if(dst == NULL || src == NULL) {
        ESP_LOGW(TAG, "benchmark 缓冲分配失败，跳过");
        heap_caps_free(dst);
        heap_caps_free(src);
        return;
    }
    for(size_t i = 0; i < bytes; i += 2) {
        src[i] = (uint8_t)(i * 7);
        src[i + 1] = (uint8_t)(i * 13);
    }
    memset(dst, 0x20, bytes);

    lv_area_t full = { .x1 = 0, .y1 = 0, .x2 = BENCH_W - 1, .y2 = BENCH_H - 1 };

    ESP_LOGI(TAG, "==== PPA benchmark（%d 次平均，dst=内部 RAM，src=PSRAM） ====", BENCH_N);

    /* CPU 纯色填充 */
    int64_t t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        lv_draw_sw_blend_fill_dsc_t d;
        lv_memzero(&d, sizeof(d));
        d.dest_buf = dst;
        d.dest_w = BENCH_W;
        d.dest_h = BENCH_H;
        d.dest_stride = BENCH_W * 2;
        d.opa = LV_OPA_COVER;
        d.color = lv_color_hex(0x123456);
        d.relative_area = full;
        lv_draw_sw_blend_color_to_rgb565(&d);
    }
    int64_t cpu_fill = (esp_timer_get_time() - t0) / BENCH_N;

#if CONFIG_UI_PPA_FILL_MIN_PX > 0
    t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        ppa_fill_rgb565(dst, &full, &full, lv_color_hex(0x123456));
    }
    int64_t ppa_fill = (esp_timer_get_time() - t0) / BENCH_N;
    ESP_LOGI(TAG, "fill  460x46 (21160 px): CPU %4lld us | PPA %4lld us",
             (long long)cpu_fill, (long long)ppa_fill);
#else
    ESP_LOGI(TAG, "fill  460x46 (21160 px): CPU %4lld us | PPA 未启用（fill 门槛=0）",
             (long long)cpu_fill);
#endif

    /* 不透明图像混合（CPU 走整行 memcpy，是 PPA 最不划算的场景） */
    t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        lv_draw_sw_blend_image_dsc_t d;
        lv_memzero(&d, sizeof(d));
        d.dest_buf = dst;
        d.dest_w = BENCH_W;
        d.dest_h = BENCH_H;
        d.dest_stride = BENCH_W * 2;
        d.src_buf = src;
        d.src_stride = BENCH_W * 2;
        d.src_color_format = LV_COLOR_FORMAT_RGB565;
        d.opa = LV_OPA_COVER;
        d.blend_mode = LV_BLEND_MODE_NORMAL;
        d.relative_area = full;
        d.src_area = full;
        lv_draw_sw_blend_image_to_rgb565(&d);
    }
    int64_t cpu_cover = (esp_timer_get_time() - t0) / BENCH_N;

    t0 = esp_timer_get_time();
    for(int i = 0; i < BENCH_N; i++) {
        ppa_blend_rgb565(dst, &full, src, &full, BENCH_W, &full, LV_OPA_COVER);
    }
    int64_t ppa_cover = (esp_timer_get_time() - t0) / BENCH_N;
    ESP_LOGI(TAG, "cover 460x46 (21160 px): CPU %4lld us | PPA %4lld us",
             (long long)cpu_cover, (long long)ppa_cover);

    /* 半透明图像混合：找出 PPA 划算的面积拐点 */
    bench_alpha(BENCH_W, BENCH_H, dst, src);
    bench_alpha(BENCH_W, BENCH_H / 2, dst, src);
    bench_alpha(BENCH_W / 4, BENCH_H, dst, src);
    bench_alpha(BENCH_W / 8, BENCH_H, dst, src);

    heap_caps_free(dst);
    heap_caps_free(src);
}
#else
void lvgl_ppa_accel_benchmark(void)
{
}
#endif /* CONFIG_UI_PPA_BENCH */

#else /* !CONFIG_UI_PPA_ACCEL */

void lvgl_ppa_accel_init(lv_display_t *disp)
{
    (void)disp;
}

void lvgl_ppa_accel_benchmark(void)
{
}

#endif /* CONFIG_UI_PPA_ACCEL */
