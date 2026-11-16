/*******************************************************************************
 * font_npf.c
 * NPF1 外置全字库加载器：mmap font 分区 → 填 lv_font_fmt_txt_dsc_t → 交给 LVGL 官方回调。
 *
 * ── 为什么 LVGL 9.4 能直接吃"预烘焙"的镜像 ────────────────────────────────
 * 9.4 的 fmt_txt 运行期只依赖三样东西（见 managed_components/lvgl__lvgl/src/font/
 * lv_font_fmt_txt.h）：
 *   1. `lv_font_fmt_txt_glyph_dsc_t`：LARGE=1 时是 16B 定长
 *      (u32 bitmap_index, u32 adv_w, u16 box_w, u16 box_h, i16 ofs_x, i16 ofs_y)，
 *      与 8.4 逐字段相同 → 可以在构建期按 LE 直接写进镜像（下面的 _Static_assert 锁死）；
 *   2. `lv_font_fmt_txt_cmap_t`：含 2 个指针，必须在 RAM 里 —— 我们只拷贝这
 *      一百多个结构体（183 个子表 ≈ 4.4KB），指针指向 mmap 区；
 *   3. 字形位图：PLAIN 格式下 `lv_font_get_bitmap_fmt_txt()` 把 bpp 位图
 *      **展开成 A8** 写进调用方给的 draw_buf（9.x 新增 stride/raw bitmap 后不再
 *      直接返回原指针），开销与内嵌位图字库完全相同；镜像里的位图按
 *      "逐字形字节对齐、无压缩、stride=0" 存放即可。
 * 差别只在结构体细节：9.4 的 dsc 多了 `stride` 字段（镜像里 stride=0，运行时显式
 * 置 0）、`lv_font_t` 多了 release_glyph/kerning/static_bitmap/user_data（memset 0
 * 即为默认行为）；cmap 的 range/list 长度仍是 u16（每子表 ≤65535，全字库最大
 * 20,902，安全）。本文件的字段偏移与镜像格式一一对应。
 *
 * ── 内存账 ────────────────────────────────────────────────────────────────
 * 非 XIP 构建：常驻 RAM ≈ sizeof(lv_font_t) + sizeof(lv_font_fmt_txt_dsc_t)
 *   + 4.4KB cmap 数组；4.4MB 位图与 538KB glyph_dsc 全在 Flash（只占 72 个
 *   64KB MMU 页）。
 * XIP-from-PSRAM 构建（CONFIG_SPIRAM_XIP_FROM_PSRAM）：镜像整块拷到 PSRAM
 *   （4.65MB），mmap 立刻解除——原因是有 flash mmap 时每次 flash 写都要关外部
 *   cache，而 .text/.rodata 已在 PSRAM，会让另一核直接 fault。详见
 *   ideas_and_exps/XIP_FROM_PSRAM.md。
 ******************************************************************************/

#include "font_npf.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "font_npf";

/* ── NPF1 镜像布局（与 tools/font/bin_font_prebake.py 严格一致，全部小端）────────
 * 头 64B： +0 magic "NPF1" | +4 version u16 | +6 face_count u16 | +8 flags u16
 *          +10 max_face u16 | +12 crc32 u32 | +16 total_size u32 | +20 保留
 * face 48B：+0 size_px u16 | +2 bpp u8 | +3 cmap_num u8 | +4 flags u16
 *          +6 kern_class u8 | +8 glyph_count u32 | +12 dsc_off | +16 cmap_off
 *          +20 lists_off | +24 bitmap_off | +28 bitmap_size | +32 kern_off
 *          +36 line_height i16 | +38 base_line i16 | +40 subpx u8
 *          +41 underline_position i8 | +42 underline_thickness u8 | +44 kern_scale u16
 * cmap 20B：+0 range_start u32 | +4 range_length u16 | +6 glyph_id_start u16
 *          +8 unicode_list_off u32 | +12 glyph_id_ofs_off u32 | +16 list_length u16
 *          +18 type u8 */
#define NPF_HDR_SIZE      64
#define NPF_FACE_SIZE     48
#define NPF_CMAP_SIZE     20
#define NPF_OFF_MAGIC     0
#define NPF_OFF_VERSION   4
#define NPF_OFF_FACES     6
#define NPF_OFF_TOTAL     16
#define NPF_NO_OFFSET     0xFFFFFFFFu
#define NPF_FACE_BASE     NPF_HDR_SIZE

/* 离线写的 glyph_dsc 必须与 LVGL 的 LARGE 布局逐字节一致，否则字形全乱。
   任何一条变了都说明 LVGL 版本/编译选项漂移，必须重新预烘焙镜像。
   （NPF 的 LARGE 布局在 8.4 / 9.4 上一致，这也是本镜像能在两条分支间复用的前提。） */
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 16,
               "glyph_dsc size mismatch (check LV_FONT_FMT_TXT_LARGE / LVGL version)");
_Static_assert(offsetof(lv_font_fmt_txt_glyph_dsc_t, bitmap_index) == 0, "glyph_dsc layout changed");
_Static_assert(offsetof(lv_font_fmt_txt_glyph_dsc_t, adv_w) == 4, "glyph_dsc layout changed");
_Static_assert(offsetof(lv_font_fmt_txt_glyph_dsc_t, box_w) == 8, "glyph_dsc layout changed");
_Static_assert(offsetof(lv_font_fmt_txt_glyph_dsc_t, ofs_x) == 12, "glyph_dsc layout changed");
/* 镜像里的 cmap 记录固定 20B（见上方布局注释），运行时逐字段读进结构体，
   所以**不依赖** sizeof(lv_font_fmt_txt_cmap_t)（9.4 的 enum 对齐可能让它变 24B）。
   下面只锁"镜像格式要用到的两个字段偏移"，它们 8.4 / 9.4 相同。 */
_Static_assert(offsetof(lv_font_fmt_txt_cmap_t, unicode_list) == 8, "cmap layout changed");
_Static_assert(offsetof(lv_font_fmt_txt_cmap_t, list_length) == 16, "cmap layout changed");

/* 挂载结果（全部静态，常驻 RAM 只有这几百字节 + cmaps 数组） */
static lv_font_t s_font;
static lv_font_fmt_txt_dsc_t s_dsc;
static lv_font_fmt_txt_cmap_t *s_cmaps;
static const esp_partition_t *s_part;
static esp_partition_mmap_handle_t s_mmap_handle;
static const uint8_t *s_base;
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
static uint8_t *s_copy;            /* XIP 构建：镜像的 PSRAM 副本（mmap 已解除） */
#endif
static uint32_t s_glyph_count;
static bool s_ready;
static esp_err_t s_last_err = ESP_ERR_INVALID_STATE;

/* 小端读取（镜像与架构同序，但显式拼装可避免对齐假设，Xtensa 上也更快） */
static inline uint16_t npf_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t npf_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int16_t npf_i16(const uint8_t *p)
{
    return (int16_t)npf_u16(p);
}

esp_err_t font_npf_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (s_part != NULL) {
        /* 已经尝试过并失败：不要每次调用都重试（避免反复刷日志） */
        return s_last_err;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "font");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "没有 font 分区（检查 partitions.csv），回退内嵌字库");
        s_last_err = ESP_ERR_NOT_FOUND;
        return s_last_err;
    }

    uint8_t hdr[NPF_HDR_SIZE];
    esp_err_t err = esp_partition_read(s_part, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读 font 分区头失败: %s", esp_err_to_name(err));
        s_last_err = err;
        return err;
    }
    if (memcmp(hdr + NPF_OFF_MAGIC, "NPF1", 4) != 0) {
        ESP_LOGW(TAG, "font 分区里不是 NPF1 镜像（多半是旧 TTF 或没刷字体），回退内嵌字库");
        s_last_err = ESP_ERR_INVALID_STATE;
        return s_last_err;
    }

    uint16_t version = npf_u16(hdr + NPF_OFF_VERSION);
    uint16_t faces = npf_u16(hdr + NPF_OFF_FACES);
    uint32_t total = npf_u32(hdr + NPF_OFF_TOTAL);
    if (version != FONT_NPF_VERSION || faces < 1 || total < NPF_HDR_SIZE + NPF_FACE_SIZE ||
        total > s_part->size) {
        ESP_LOGW(TAG, "NPF1 头非法（version=%u faces=%u total=%" PRIu32 " 分区=%" PRIu32 "），回退内嵌字库",
                 version, faces, total, (uint32_t)s_part->size);
        s_last_err = ESP_ERR_INVALID_STATE;
        return s_last_err;
    }

    /* 【2026-11-16，XIP-from-PSRAM 适配】不能长期保留 font 分区的 flash mmap：
       1) 只要还有 flash mmap，每次 flash 写都要关掉外部 cache
          （spi_flash_os_func_app.c 的 flash_mmap_remain() 分支）。XIP 模式下
          .text/.rodata 已经在 PSRAM，一关 cache，另一颗核的 MP3 解码就会
          access fault（2026-11-16 实测：开机几秒内 Core1 随机崩溃）；
       2) 4.65MB 的 mmap 还要占 71 个 MMU 页，和 XIP 映射、PSRAM 堆抢页表。
       做法：先用 mmap 快速读（走 cache，比逐块 esp_partition_read 快），
       整块 memcpy 到 PSRAM，再 munmap 把页表还回去。运行时字形直接从 PSRAM
       读——既不受 flash 写窗口影响，也比 cache 映射的 flash 读更快。
       代价：PSRAM 多占 total 字节（约 4.4MB），开机多一次 memcpy（~20ms 量级）。
       非 XIP 构建保持 mmap（省 4.4MB PSRAM，且没有上述问题）。 */
    const uint8_t *mapped = NULL;
    err = esp_partition_mmap(s_part, 0, total, ESP_PARTITION_MMAP_DATA,
                             (const void **)&mapped, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "font 分区 mmap 失败: %s", esp_err_to_name(err));
        s_last_err = err;
        return err;
    }

#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    uint8_t *copy = heap_caps_aligned_alloc(64, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy != NULL) {
        const int64_t t0 = esp_timer_get_time();
        memcpy(copy, mapped, total);
        /* esp_partition_munmap 无返回值：无论成败都以释放为准，
           失败也只会在极端情况下多留几页 MMU（不影响功能）。 */
        esp_partition_munmap(s_mmap_handle);
        s_copy = copy;
        s_base = copy;
        ESP_LOGI(TAG, "font 镜像已拷入 PSRAM：%u B，%lld ms，释放 %u 个 64KB MMU 页",
                 (unsigned)total, (long long)((esp_timer_get_time() - t0) / 1000),
                 (unsigned)((total + CONFIG_MMU_PAGE_SIZE - 1) / CONFIG_MMU_PAGE_SIZE));
    } else {
        s_base = mapped;
        ESP_LOGW(TAG, "PSRAM 分配 %u B 失败，继续使用 flash 映射（flash 写会关 cache）",
                 (unsigned)total);
    }
#else
    s_base = mapped;
#endif

    const uint8_t *face = s_base + NPF_FACE_BASE;
    uint16_t size_px = npf_u16(face + 0);
    uint8_t bpp = face[2];
    uint8_t cmap_num = face[3];
    uint32_t glyph_count = npf_u32(face + 8);
    uint32_t dsc_off = npf_u32(face + 12);
    uint32_t cmap_off = npf_u32(face + 16);
    uint32_t bitmap_off = npf_u32(face + 24);
    uint32_t bitmap_size = npf_u32(face + 28);
    int16_t line_height = npf_i16(face + 36);
    int16_t base_line = npf_i16(face + 38);
    uint8_t subpx = face[40];
    int8_t ul_pos = (int8_t)face[41];
    uint8_t ul_thk = face[42];
    uint16_t kern_scale = npf_u16(face + 44);

    /* 偏移合法性（截断/刷写不全都表现为这里失败，比后面花屏好定位） */
    bool bad = (bpp != 2) || (cmap_num == 0) || (glyph_count == 0) || (line_height <= 0) ||
               (dsc_off + glyph_count * 16u > total) ||
               (cmap_off + (uint32_t)cmap_num * NPF_CMAP_SIZE > total) ||
               (bitmap_off + bitmap_size > total);
    if (bad) {
        ESP_LOGW(TAG, "NPF1 face 记录非法（bpp=%u cmap=%u glyphs=%" PRIu32 "），回退内嵌字库",
                 bpp, cmap_num, glyph_count);
        s_last_err = ESP_ERR_INVALID_STATE;
        return s_last_err;
    }

    /* cmap 结构体含指针，必须在 RAM：优先内部 RAM（4.4KB，越小越好；失败再退默认堆） */
    size_t cmaps_bytes = (size_t)cmap_num * sizeof(lv_font_fmt_txt_cmap_t);
    s_cmaps = heap_caps_malloc(cmaps_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_cmaps == NULL) {
        s_cmaps = malloc(cmaps_bytes);
    }
    if (s_cmaps == NULL) {
        ESP_LOGW(TAG, "分配 cmap 表（%u B）失败，回退内嵌字库", (unsigned)cmaps_bytes);
        s_last_err = ESP_ERR_NO_MEM;
        return s_last_err;
    }

    for (uint32_t i = 0; i < cmap_num; i++) {
        const uint8_t *rec = s_base + cmap_off + i * NPF_CMAP_SIZE;
        lv_font_fmt_txt_cmap_t *c = &s_cmaps[i];
        uint32_t uni_off = npf_u32(rec + 8);
        uint32_t ofs_off = npf_u32(rec + 12);
        c->range_start = npf_u32(rec + 0);
        c->range_length = npf_u16(rec + 4);
        c->glyph_id_start = npf_u16(rec + 6);
        c->unicode_list = (uni_off == NPF_NO_OFFSET) ? NULL
                                                     : (const uint16_t *)(s_base + uni_off);
        c->glyph_id_ofs_list = (ofs_off == NPF_NO_OFFSET) ? NULL : (const void *)(s_base + ofs_off);
        c->list_length = npf_u16(rec + 16);
        c->type = rec[18];
    }

    /* dsc：位图与 glyph_dsc 直接指向 mmap 区；kern 丢弃（镜像里也没生成） */
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.glyph_bitmap = s_base + bitmap_off;
    s_dsc.glyph_dsc = (const lv_font_fmt_txt_glyph_dsc_t *)(const void *)(s_base + dsc_off);
    s_dsc.cmaps = s_cmaps;
    s_dsc.kern_dsc = NULL;
    s_dsc.kern_scale = kern_scale;
    s_dsc.cmap_num = cmap_num;
    s_dsc.bpp = bpp;
    s_dsc.kern_classes = 0;
    s_dsc.bitmap_format = LV_FONT_FMT_TXT_PLAIN;
    /* 9.4 新增：每行位图的字节对齐（0 = 不对齐、无 padding）。镜像里的位图是
       按字形紧排的 bpp2 数据，必须显式置 0；否则 9.4 的取位图路径会按
       LV_ROUND_UP 算行宽、把位图读歪。 */
    s_dsc.stride = 0;

    /* 字体描述符：fallback 指向内置 montserrat_24，补 LV_SYMBOL_*（PUA 图标）
       ⚠ 9.4 的 lv_font_t 多了 release_glyph / kerning / static_bitmap / user_data：
         memset 0 后 release_glyph=NULL（LVGL 的 lv_font_glyph_release_draw_data 会
         先判空）、kerning=LV_FONT_KERNING_NORMAL、static_bitmap=0，均为我们要的行为。 */
    memset(&s_font, 0, sizeof(s_font));
    s_font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    s_font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    s_font.line_height = line_height;
    s_font.base_line = base_line;
    s_font.subpx = (lv_font_subpx_t)subpx;
    s_font.underline_position = ul_pos;
    s_font.underline_thickness = ul_thk;
    s_font.dsc = &s_dsc;
    s_font.fallback = &lv_font_montserrat_24;

    s_glyph_count = glyph_count;
    s_ready = true;
    s_last_err = ESP_OK;

    ESP_LOGI(TAG, "NPF1 全字库已挂载: %" PRIu32 "px bpp%u, %" PRIu32 " 个字形, %u 个 cmap, "
             "镜像 %" PRIu32 "B @0x%" PRIX32 ", 行高 %d",
             (uint32_t)size_px, bpp, glyph_count, cmap_num, total, (uint32_t)s_part->address,
             (int)line_height);
    return ESP_OK;
}

const lv_font_t *font_npf_get(void)
{
    return s_ready ? &s_font : NULL;
}

uint32_t font_npf_glyph_count(void)
{
    return s_glyph_count;
}
