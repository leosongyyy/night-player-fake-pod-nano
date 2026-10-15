#include "core/image/cover_jpeg.h"
#include "core/utils/mem_trace.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"

static const char *TAG = "cover_jpeg";

/* 像素缓冲分配：优先 PSRAM（8MB 充足，避免挤占内部 RAM），内部 RAM 兜底 */
static uint8_t *cover_alloc(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        /* ⚠ 内存诊断（FUTURE_PLAN 第 34 点）：460x460 RGB565 = 423KB 绝不能落
           内部 RAM（内部 DRAM 总容量都不够），PSRAM 失败时回退几乎必然失败 */
        ESP_LOGW(TAG, "PSRAM alloc %u B failed, fallback to default heap",
                 (unsigned)size);
        buf = malloc(size);
    }
    /* 打印落点（CONFIG_MEM_TRACE）：460x460 RGB565 = 423KB 绝不能落内部 RAM */
    mem_trace_ptr(TAG, "pixel", size, buf);
    return buf;
}

esp_err_t cover_jpeg_decode(const uint8_t *jpeg, uint32_t len, lv_image_dsc_t *out)
{
    if (jpeg == NULL || len == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* 先只解析头部拿尺寸（不分配大缓冲），确认图片信息合法 */
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            /* ⚠ LVGL 9 迁移：v8 靠 CONFIG_LV_COLOR_16_SWAP=y 让渲染管线直接
               消费"高字节在前"的 RGB565，故那时这里要 swap_color_bytes=1；
               v9 改为渲染本机字节序、在 flush 回调里整体交换一次
               （core/ui/lvgl_port.c），所以解码必须输出本机（小端）字节序，
               这里改回 0，否则颜色会整体错乱（等于被交换两次）。 */
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
    /* 防御异常尺寸（正常为 460x460；上限 512 防超大分配） */
    if (info.width > 512 || info.height > 512) {
        ESP_LOGE(TAG, "jpeg size too large: %ux%u", info.width, info.height);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 分配像素缓冲并解码 */
    size_t pixel_bytes = (size_t)info.width * info.height * 2;
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

    /* 封装为 LVGL 图片（LVGL 9：描述符类型为 lv_image_dsc_t，色彩格式用
       LV_COLOR_FORMAT_* 枚举，需要 magic，且显式给出 stride = 宽 × 2B） */
    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.w = outimg.width;
    out->header.h = outimg.height;
    out->header.cf = LV_COLOR_FORMAT_RGB565;
    out->header.flags = 0;
    out->header.stride = outimg.width * 2;
    out->data_size = pixel_bytes;
    out->data = buf;

    ESP_LOGI(TAG, "decoded %ux%u (%u B)",
             (unsigned)outimg.width, (unsigned)outimg.height,
             (unsigned)outimg.output_len);
    return ESP_OK;
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
