#include "core/image/cover_jpeg.h"
#include "core/utils/mem_trace.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"          /* 软件解码（espressif/esp_jpeg，tjpgd） */
#include "sdkconfig.h"

/* 硬件 JPEG 解码器（ESP32-S31 内置 codec，esp_driver_jpeg）：
   用 CONFIG_UI_COVER_HW_JPEG 控制，见 main/Kconfig.projbuild。 */
#if CONFIG_UI_COVER_HW_JPEG && SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#define COVER_JPEG_HW_ENABLED   1
#else
#define COVER_JPEG_HW_ENABLED   0
#endif

static const char *TAG = "cover_jpeg";

/* 像素缓冲分配：优先 PSRAM（8MB 充足，避免挤占内部 RAM），内部 RAM 兜底。
   ⚠⚠ 必须按 64B 对齐（与 LV_DRAW_BUF_ALIGN / LV_ATTRIBUTE_MEM_ALIGN_SIZE 一致）：
   封面会被 LVGL 当作 img->data 包成 draw_buf（lv_draw_buf_from_image），
   未对齐时 lv_draw_buf_init 每次都刷 "Data is not aligned, ignored" ——
   实测切歌后（真实封面出现在正在播放页）滑动时刷屏级日志，115200 波特下
   一条 ~5ms，直接把 UI 卡死。这里对齐后警告消失。 */
#define COVER_PIXEL_ALIGN  64

/* 封面解码耗时 > 这个值就打 WARN（460x460 软解 100~300ms 是正常的，只在
   异常慢时提示）。实际耗时无论快慢都会打一条 INFO（见 cover_jpeg_decode）。 */
#define COVER_DECODE_SLOW_MS   400

static uint8_t *cover_alloc(size_t size)
{
    uint8_t *buf = heap_caps_aligned_alloc(COVER_PIXEL_ALIGN, size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        /* ⚠ 内存诊断（FUTURE_PLAN 第 34 点）：460x460 RGB565 = 423KB 绝不能落
           内部 RAM（内部 DRAM 总容量都不够），PSRAM 失败时回退几乎必然失败 */
        ESP_LOGW(TAG, "PSRAM alloc %u B failed, fallback to default heap",
                 (unsigned)size);
        buf = heap_caps_aligned_alloc(COVER_PIXEL_ALIGN, size, MALLOC_CAP_8BIT);
    }
    /* 打印落点（CONFIG_MEM_TRACE）：460x460 RGB565 = 423KB 绝不能落内部 RAM */
    mem_trace_ptr(TAG, "pixel", size, buf);
    return buf;
}

/* 把解码产物封装成 LVGL 图片描述符（两条解码路径共用的收尾）。
   ⚠ RGB565 字节序：输出必须是**本机小端**（LVGL 9 渲染缓冲的原生字节序），
     v9 已移除 v8 的 CONFIG_LV_COLOR_16_SWAP，改由移植层在 flush 回调里
     对整块缓冲调用 lv_draw_sw_rgb565_swap()（见 core/ui/lvgl_port.c）。 */
static void cover_fill_image(lv_image_dsc_t *out, uint8_t *buf,
                             uint32_t width, uint32_t height)
{
    out->header.magic  = LV_IMAGE_HEADER_MAGIC;
    out->header.w      = width;
    out->header.h      = height;
    out->header.cf     = LV_COLOR_FORMAT_RGB565;
    out->header.flags  = 0;
    out->header.stride = width * 2;
    out->data_size     = (uint32_t)width * height * 2;
    out->data          = buf;
}

/* 解码结果签名：把图片稀疏采样后算 RGB 三通道均值并打印中心/左上角像素。
   用途：对比"硬件路径 vs 软件路径"的输出是否一致（颜色通道是否错位、行是否
   错切），换芯片/改解码配置时可直接对账。
   ⚠ 两条路径的 YUV→RGB 舍入不同，数值允许 ±3 的差别（2026-11 实测同一张
   460x460 封面：hw avgR/G/B = 28/55/25、sw = 27/52/24，中心像素 0xffff vs
   0xffdd，左上角 0xc638 vs 0xbdd7）；R/B 对调或行错位会差出几十以上。 */
static void cover_log_signature(const char *how, const lv_image_dsc_t *img)
{
    const uint16_t *px = (const uint16_t *)img->data;
    const uint32_t w = img->header.w;
    const uint32_t h = img->header.h;
    if (px == NULL || w == 0 || h == 0) {
        return;
    }
    uint32_t n = 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
    for (uint32_t y = 0; y < h; y += 29) {
        for (uint32_t x = 0; x < w; x += 29) {
            uint16_t v = px[(size_t)y * w + x];
            sum_r += (v >> 11) & 0x1F;
            sum_g += (v >> 5) & 0x3F;
            sum_b += v & 0x1F;
            n++;
        }
    }
    uint32_t c = px[(size_t)(h / 2) * w + (w / 2)];
    ESP_LOGD(TAG, "sig[%s] %ux%u samples=%u avgR=%u avgG=%u avgB=%u "
                  "center=0x%04x (r%u g%u b%u) tl=0x%04x",
             how, (unsigned)w, (unsigned)h, (unsigned)n,
             (unsigned)(sum_r / n), (unsigned)(sum_g / n), (unsigned)(sum_b / n),
             (unsigned)c, (unsigned)((c >> 11) & 0x1F),
             (unsigned)((c >> 5) & 0x3F), (unsigned)(c & 0x1F),
             (unsigned)px[0]);
}

/* ------------------------------------------------------------------ */
/* 软件解码（espressif/esp_jpeg / tjpgd）—— 兜底路径                    */
/* ------------------------------------------------------------------ */

static esp_err_t cover_decode_sw(const uint8_t *jpeg, uint32_t len,
                                 lv_image_dsc_t *out)
{
    /* 先只解析头部拿尺寸（不分配大缓冲），确认图片信息合法 */
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            /* ⚠ 必须 0：v9 由 flush 回调统一交换字节序，这里再交换会错乱 */
            .swap_color_bytes = 0,
        },
    };
    esp_jpeg_image_output_t info;
    esp_err_t r = esp_jpeg_get_image_info(&cfg, &info);
    if (r != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGE(TAG, "jpeg info failed: %s (w=%u h=%u)",
                 esp_err_to_name(r), (unsigned)info.width, (unsigned)info.height);
        return (r != ESP_OK) ? r : ESP_FAIL;
    }
    /* 防御异常尺寸：内嵌/目录封面常见 1000x1000，允许到 4096 并在
       超过 1024 时用软件解码的 1/2、1/4 缩放（见下方 scale 选择）。
       最终像素尺寸不超过 1024x1024，避免 PSRAM 峰值过大。 */
    if (info.width > 4096 || info.height > 4096) {
        ESP_LOGE(TAG, "jpeg size too large: %ux%u", (unsigned)info.width, (unsigned)info.height);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t max_dim = info.width > info.height ? info.width : info.height;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    uint32_t div = 1;
    if (max_dim > 2048) {
        scale = JPEG_IMAGE_SCALE_1_4;
        div = 4;
    } else if (max_dim > 1024) {
        scale = JPEG_IMAGE_SCALE_1_2;
        div = 2;
    }
    cfg.out_scale = scale;

    /* 缩放后尺寸按向上取整估算；额外留 16px 余量吸收 MCU 对齐差异 */
    uint32_t out_w = (info.width + div - 1) / div;
    uint32_t out_h = (info.height + div - 1) / div;
    size_t pixel_bytes = (size_t)(out_w + 16) * (out_h + 16) * 2;
    uint8_t *buf = cover_alloc(pixel_bytes);
    if (buf == NULL) {
        ESP_LOGE(TAG, "alloc %u B failed", (unsigned)pixel_bytes);
        return ESP_ERR_NO_MEM;
    }
    cfg.outbuf = buf;
    cfg.outbuf_size = pixel_bytes;

    esp_jpeg_image_output_t outimg;
    r = esp_jpeg_decode(&cfg, &outimg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decode failed: %s", esp_err_to_name(r));
        free(buf);
        return r;
    }
    if (outimg.output_len < pixel_bytes) {
        /* 尺寸与解码不一致（理论上同 info）；置灰剩余区避免脏数据 */
        memset(buf + outimg.output_len, 0, pixel_bytes - outimg.output_len);
    }

    cover_fill_image(out, buf, outimg.width, outimg.height);
    ESP_LOGD(TAG, "sw decoded %ux%u (%u B)",
             (unsigned)outimg.width, (unsigned)outimg.height,
             (unsigned)outimg.output_len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 硬件解码（ESP32-S31 内置 JPEG codec + DMA2D）                       */
/* ------------------------------------------------------------------ */

#if COVER_JPEG_HW_ENABLED

/* 解码引擎：进程内单实例，懒创建后常驻。
   ⚠ 只在 cover_load 任务（Core1）里调用，天然串行，不需要额外加锁；
     esp_driver_jpeg 内部还有 codec_mutex 兜底。
   ⚠ 创建失败只试一次（s_engine_tried），之后整机走软件解码，避免每首歌
     都重试一遍并刷日志。 */
static jpeg_decoder_handle_t s_engine       = NULL;
static bool                  s_engine_tried = false;

/* MCU 对齐尺寸：硬件按 MCU（最小编码单元）整块输出，YUV420/422 的宽度会被
   向上对齐到 16、YUV420 的高度也对齐到 16（见 IDF 的
   jpeg_parse_marker.c：mcux = hi[0]*8、mcuy = vi[0]*8）。
   ⚠ 这一步不能省：jpeg_decoder_process 会按**对齐后**的尺寸校验并写满输出
     缓冲（real_size = process_h * process_v * bpp/8），缓冲给小了会直接
     ESP_ERR_INVALID_ARG。 */
static void cover_hw_padded_size(jpeg_down_sampling_type_t sampling,
                                 uint32_t width, uint32_t height,
                                 uint32_t *padded_w, uint32_t *padded_h)
{
    uint32_t mcu_x = 8;
    uint32_t mcu_y = 8;
    if (sampling == JPEG_DOWN_SAMPLING_YUV422) {
        mcu_x = 16;     /* H=2 V=1 */
    } else if (sampling == JPEG_DOWN_SAMPLING_YUV420) {
        mcu_x = 16;     /* H=2 V=2 */
        mcu_y = 16;
    }
    *padded_w = (width + mcu_x - 1) / mcu_x * mcu_x;
    *padded_h = (height + mcu_y - 1) / mcu_y * mcu_y;
}

/* 懒创建解码引擎。返回 NULL 表示硬件路径不可用（此后一直走软件解码）。 */
static jpeg_decoder_handle_t cover_hw_engine(void)
{
    if (s_engine != NULL) {
        return s_engine;
    }
    if (s_engine_tried) {
        return NULL;
    }
    s_engine_tried = true;

    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,     /* 0 = 驱动自选（JPEG 允许 LowMed） */
        .timeout_ms    = 500,   /* 460x460 实测 10ms 量级，500ms 足够宽松 */
        .flags = { .allow_pd = 0 },   /* 不进睡眠保持（本项目不做 deep sleep） */
    };
    esp_err_t r = jpeg_new_decoder_engine(&cfg, &s_engine);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "hw jpeg engine create failed: %s，改用软件解码",
                 esp_err_to_name(r));
        s_engine = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "hw jpeg decoder ready (ESP32-S31 JPEG codec)");
    return s_engine;
}

static esp_err_t cover_decode_hw(const uint8_t *jpeg, uint32_t len,
                                 lv_image_dsc_t *out)
{
    jpeg_decoder_handle_t engine = cover_hw_engine();
    if (engine == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 1. 先读头部（纯解析、不用硬件），拿尺寸与采样方式 */
    jpeg_decode_picture_info_t info;
    esp_err_t r = jpeg_decoder_get_info(jpeg, len, &info);
    if (r != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "hw jpeg info failed: %s", esp_err_to_name(r));
        return (r != ESP_OK) ? r : ESP_FAIL;
    }
    /* 与软件路径一致的防御；另外硬件只支持"总像素数能被 8 整除"的图 */
    if (info.width > 512 || info.height > 512 ||
        (info.width * info.height) % 8 != 0) {
        ESP_LOGW(TAG, "hw jpeg unsupported size: %ux%u",
                 (unsigned)info.width, (unsigned)info.height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 2. 按 MCU 对齐尺寸分配输出缓冲。
       ⚠ 必须用 jpeg_alloc_decoder_mem：它同时满足"地址按 cache 行对齐"
         与"大小按 cache 行对齐"两条要求，jpeg_decoder_process 会校验；
         返回的 allocated_size 是向上对齐后的实际大小，必须原样传回去。
       ⚠ 该缓冲返回的是普通 heap 指针，用 free() 释放（与 cover_jpeg_free
         一致），所以它可以一直留作 LVGL 图片数据。 */
    uint32_t padded_w = 0;
    uint32_t padded_h = 0;
    cover_hw_padded_size(info.sample_method, info.width, info.height,
                         &padded_w, &padded_h);

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_size = 0;
    uint8_t *buf = jpeg_alloc_decoder_mem((size_t)padded_w * padded_h * 2,
                                          &mem_cfg, &alloc_size);
    if (buf == NULL) {
        ESP_LOGE(TAG, "hw alloc %u B failed",
                 (unsigned)((size_t)padded_w * padded_h * 2));
        return ESP_ERR_NO_MEM;
    }
    mem_trace_ptr(TAG, "pixel(hw)", alloc_size, buf);

    /* 3. 解码：输出 RGB565。
       ⚠ rgb_order 取 BGR = 输出**小端** RGB565，正好是 LVGL 9 渲染缓冲的
         原生字节序（与软件路径 swap_color_bytes=0 等价）。取 RGB（大端）会让
         红蓝通道整体错位。
       ⚠ conv_std 取 BT601：JFIF/JPEG 的标准 YUV→RGB 矩阵就是 BT.601。 */
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    r = jpeg_decoder_process(engine, &dec_cfg, jpeg, len, buf, alloc_size,
                             &out_size);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "hw jpeg decode failed: %s，本次退回软件解码",
                 esp_err_to_name(r));
        free(buf);
        return r;
    }

    /* 4. 压实每行：硬件按 padded_w 宽输出（460 → 464），而 LVGL 的绘制路径
         要求**紧凑**布局——PPA 的图片绘制单元直接用 pic_w = 图片宽度读源
         缓冲（见 lv_draw_ppa_img.c，它根本不看 header.stride），所以带
         padding 的 stride 会画出斜切/错行的图。这里就地把每行往上挪成紧凑。
       ⚠ 顺序必须是**从第 1 行往后**：第 y 行的目的地 [y*row, (y+1)*row)
         与第 y+1 行的源 [（y+1)*pitch, …) 不相交（pitch > row），而 memmove
         自身能处理"目的 < 源"的重叠。反过来从后往前会踩掉未搬的源数据。 */
    if (padded_w != info.width) {
        const size_t row   = (size_t)info.width * 2;
        const size_t pitch = (size_t)padded_w * 2;
        for (uint32_t y = 1; y < info.height; y++) {
            memmove(buf + (size_t)y * row, buf + (size_t)y * pitch, row);
        }
    }

    cover_fill_image(out, buf, info.width, info.height);
    ESP_LOGD(TAG, "hw decoded %ux%u (padded %ux%u, %u B)",
             (unsigned)info.width, (unsigned)info.height,
             (unsigned)padded_w, (unsigned)padded_h, (unsigned)out_size);
    return ESP_OK;
}

#endif /* COVER_JPEG_HW_ENABLED */

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t cover_jpeg_decode(const uint8_t *jpeg, uint32_t len, lv_image_dsc_t *out)
{
    if (jpeg == NULL || len == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    int64_t t0 = esp_timer_get_time();
    esp_err_t r = ESP_ERR_NOT_SUPPORTED;
    const char *how = "sw";

#if COVER_JPEG_HW_ENABLED
    /* 硬件优先；任何失败都退回软件解码（封面不能因为硬件路径出问题而消失） */
    r = cover_decode_hw(jpeg, len, out);
    if (r == ESP_OK) {
        how = "hw";
    } else {
        memset(out, 0, sizeof(*out));
        r = cover_decode_sw(jpeg, len, out);
    }
#else
    r = cover_decode_sw(jpeg, len, out);
#endif

    if (r == ESP_OK) {
        int32_t ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGI(TAG, "decoded %ux%u (%u B) in %d ms [%s]",
                 (unsigned)out->header.w, (unsigned)out->header.h,
                 (unsigned)out->data_size, (int)ms, how);
        if (ms > COVER_DECODE_SLOW_MS) {
            ESP_LOGW(TAG, "cover decode slow: %d ms（软解 100~300ms 属正常）", (int)ms);
        }
        cover_log_signature(how, out);
    }
    return r;
}

void cover_jpeg_free(lv_image_dsc_t *img)
{
    if (img == NULL) {
        return;
    }
    if (img->data != NULL) {
        free((void *)img->data);
    }
    memset(img, 0, sizeof(*img));
}
