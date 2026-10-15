/*******************************************************************************
 * font_ttf.c
 * 系统字体的 TTF（FreeType）实现 —— 字体文件在 font 分区（裸二进制，无文件系统）。
 *
 * 本文件做三件事（详见 ideas_and_exps/FontSystem.md）：
 *   1. 注册一个只读的 lv_fs 驱动（盘符 'F'），把 LVGL 的文件操作映射到
 *      esp_partition_read，因此字体路径是 "F:/system.ttf"；
 *   2. 校验分区里的 sfnt 头并从表目录算出字体文件的真实长度；
 *      ⚠ 裸分区没有"文件长度"这种元数据，而 FreeType 的流长度是通过
 *        lv_fs_seek(END) + lv_fs_tell 得到的（lv_ftsystem.c 里就这么干），
 *        长度必须由我们自己算准，否则 FreeType 会把分区里的 0xFF 填充当数据。
 *   3. 预创建 UI 需要的各字号字体对象，供 font_ttf_get() 取用。
 *
 * 几个必须记住的点：
 *   · ⚠ 应用侧**不要**调 lv_freetype_init()：LVGL 9.4 的 lv_init() 已经初始化过
 *     FreeType（lv_init.c: lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT)），
 *     重复调用会 LV_LOG_WARN("freetype already initialized") 并返回
 *     LV_RESULT_INVALID（引擎其实是好的）——参考项目 OtoClock 踩过这个坑。
 *   · ⚠ lv_fs_drv_register() 只保存**指针**（lv_fs.c: `*new_drv = drv_p;`，
 *     不像 LVGL 8 那样拷贝结构体），所以 s_drv 必须是 static 存储期。
 *   · FreeType 的内存分配走 lv_malloc → core/ui/lvgl_mem.c → PSRAM；
 *     文件访问走 lv_fs（CONFIG_LV_FREETYPE_USE_LVGL_PORT=y）。
 ******************************************************************************/
#include "core/fonts/font_ttf.h"

#include <string.h>

#include "core/fonts/system_global_font_lite.h"
#include "core/fonts/system_smaller_font.h"
#include "core/ui/lvgl_port.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

static const char *TAG = "font_ttf";

/* ---------------- font 分区（裸二进制）相关常量 ---------------- */
#define FONT_PART_LABEL    "font"
#define FONT_PART_SUBTYPE  0x41            /* partitions.csv 里 font 的 subtype（自定义值） */
#define FONT_FS_LETTER     'F'             /* LVGL 盘符：完整路径 "F:/system.ttf" */
#define FONT_FILE_NAME     "/system.ttf"   /* lv_fs 传给 open_cb 的是**去掉盘符后**的路径 */
#define FONT_PATH          "F:" FONT_FILE_NAME

/* lv_fs 的文件缓存大小（字节，0 = 不缓存）。
   0 时每次读直接进 esp_partition_read；FreeType 取一个字形大约读 3~6 次
   （loca 表项 + glyf 头 + 字形数据），加了缓存能把这几次并成一次 flash 读，
   但要走 LVGL 的带缓存 seek 路径（lv_fs_seek_cached）。参考项目（SD 卡上读
   21MB 字体）用默认的 0 就够快，这里先保守取 0；若发现长列表首次滚动掉帧，
   可改成 4096 再实测对比。 */
#define FONT_FS_CACHE_SIZE 0

/* lv_fs 同时打开的句柄数。FreeType 对同一个 (路径, 样式, 渲染模式) 只开一个
   FT_Face，两个字号共用同一个流，所以 2 个槽足够。 */
#define FONT_OPEN_MAX      2

/* 最多预创建的字号数（见下面的 s_preload_px） */
#define FONT_SLOT_MAX      4

/* sfnt 头魔数（TrueType / 'true' / CFF-OpenType / ttc 集合） */
#define SFNT_TRUETYPE      0x00010000u
#define SFNT_TRUE          0x74727565u
#define SFNT_OTTO          0x4F54544Fu
#define SFNT_TTCF          0x74746366u

/* ==========================================================================
 * 内嵌位图回退字体
 * --------------------------------------------------------------------------
 * TTF 不可用时（分区空/文件损坏/编译期关闭）界面仍要有字，所以：
 *   · system_global_font_lite（24px，系统 UI 用字 + ASCII）**始终编译**；
 *   · system_smaller_font（20px）也在固件里。
 * 两者的字形数远少于 TTF，用户数据里的生僻字会显示不出来——这是"回退态"，
 * 不是正常工作状态，串口会有明确的 E 级日志。
 * ========================================================================== */
static const lv_font_t *fallback_font_for_px(uint16_t px)
{
    /* 20px 及以下 → 20px 点阵；其它 → 24px 点阵（当前只有这两个字号在用） */
    return (px <= UI_FONT_PX_SMALL) ? &system_smaller_font : &system_global_font_lite;
}

/* 本模块的开关：Kconfig 的应用侧开关 + LVGL 侧引擎开关，两个都要有。
   （只开 CONFIG_UI_FONT_TTF 而没开 CONFIG_LV_USE_FREETYPE 会在链接期找不到
    lv_freetype_font_create，这里提前给出明确的编译错误。） */
#if CONFIG_UI_FONT_TTF && !LV_USE_FREETYPE
#error "CONFIG_UI_FONT_TTF=y 需要 CONFIG_LV_USE_FREETYPE=y（FreeType 引擎，见 sdkconfig.defaults）"
#endif

#if !(CONFIG_UI_FONT_TTF && LV_USE_FREETYPE)

/* 编译期关闭 TTF：整个模块退化为"永远返回回退字体" */
esp_err_t font_ttf_init(void)
{
    ESP_LOGW(TAG, "CONFIG_UI_FONT_TTF=n：界面使用内嵌位图字体(%dpx/%dpx)，"
                  "font 分区与 FreeType 引擎不参与",
             UI_FONT_PX_MAIN, UI_FONT_PX_SMALL);
    return ESP_ERR_NOT_SUPPORTED;
}

const lv_font_t *font_ttf_get(uint16_t px)
{
    return fallback_font_for_px(px);
}

bool font_ttf_is_ready(void)
{
    return false;
}

#else /* CONFIG_UI_FONT_TTF && LV_USE_FREETYPE */

/* ---------------- 裸分区 lv_fs 驱动 ---------------- */
typedef struct {
    bool     in_use;
    uint32_t pos;      /* 当前绝对偏移（相对分区起点，也就是相对文件起点） */
} font_file_t;

static const esp_partition_t *s_part;               /* font 分区句柄 */
static uint32_t               s_file_size;          /* 字体文件真实长度（用于流长度） */
static const void            *s_map;                /* 分区 mmap 基址（成功则读走 memcpy） */
static esp_partition_mmap_handle_t s_map_handle;
static font_file_t            s_files[FONT_OPEN_MAX];
static lv_fs_drv_t            s_drv;                /* ⚠ 必须 static：注册时只存指针 */
static bool                   s_ready;

static struct {
    uint16_t         px;
    const lv_font_t *font;      /* NULL = 该字号创建失败（用回退字体） */
} s_slots[FONT_SLOT_MAX];

/* 预创建的字号（UI 实际用到的就是这两个，见头文件） */
static const uint16_t s_preload_px[] = { UI_FONT_PX_MAIN, UI_FONT_PX_SMALL };

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* --------------------------------------------------------------------------
 * 探测分区里的字体：校验 sfnt 头 + 遍历表目录算真实长度
 * 返回 0 表示不是可用的字体（已打印原因）
 * ------------------------------------------------------------------------ */
static uint32_t probe_font_file(void)
{
    uint8_t hdr[12];
    if (esp_partition_read(s_part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "读 font 分区头部失败");
        return 0;
    }

    uint32_t magic = rd_u32(hdr);
    if (magic != SFNT_TRUETYPE && magic != SFNT_TRUE && magic != SFNT_OTTO && magic != SFNT_TTCF) {
        ESP_LOGE(TAG, "font 分区里不是 TTF/OTF（头 4 字节 = 0x%08x）；"
                      "0xFFFFFFFF 表示分区是空的 —— 用 `idfc flash` 把 spiffs/system.ttf 烧进去",
                 (unsigned)magic);
        return 0;
    }
    if (magic == SFNT_TTCF) {
        /* 字体集合（ttc）：表目录结构不同，本函数不解析，直接把分区大小当流长度。
           本项目用的是单个 TTF，正常不会走到这里。 */
        ESP_LOGW(TAG, "字体是 ttc 集合，跳过长度校验，直接按分区大小当流长度");
        return s_part->size;
    }

    uint16_t num_tables = rd_u16(hdr + 4);
    if (num_tables == 0 || num_tables > 512) {
        ESP_LOGE(TAG, "sfnt 表数量异常：%u", (unsigned)num_tables);
        return 0;
    }

    uint32_t max_end = 0;
    uint32_t maxp_off = 0;
    uint32_t maxp_len = 0;
    bool     has_glyf = false;
    bool     has_cff = false;

    for (uint16_t i = 0; i < num_tables; i++) {
        uint8_t  rec[16];
        uint32_t off = 12u + (uint32_t)i * 16u;

        if (esp_partition_read(s_part, off, rec, sizeof(rec)) != ESP_OK) {
            ESP_LOGE(TAG, "读表目录第 %u 项失败", (unsigned)i);
            return 0;
        }

        const char *tag = (const char *)rec;
        uint32_t t_off = rd_u32(rec + 8);
        uint32_t t_len = rd_u32(rec + 12);

        /* 越界检查：表必须落在分区内（分区 4MiB，字体文件 3.55MiB） */
        if (t_off > s_part->size || t_len > s_part->size || t_off + t_len > s_part->size) {
            ESP_LOGE(TAG, "表 '%.4s' 越界（offset=%u len=%u，分区 %u B），字体文件疑似损坏",
                     tag, (unsigned)t_off, (unsigned)t_len, (unsigned)s_part->size);
            return 0;
        }
        if (t_off + t_len > max_end) max_end = t_off + t_len;

        if (memcmp(tag, "maxp", 4) == 0) { maxp_off = t_off; maxp_len = t_len; }
        if (memcmp(tag, "glyf", 4) == 0) has_glyf = true;
        if (memcmp(tag, "CFF ", 4) == 0) has_cff = true;
    }

    if (maxp_off && maxp_len >= 6) {
        uint8_t mp[6];
        if (esp_partition_read(s_part, maxp_off, mp, sizeof(mp)) == ESP_OK) {
            ESP_LOGI(TAG, "字体信息：%s，%u 张表，%u 个字形",
                     has_cff ? "CFF/OpenType" : (has_glyf ? "TrueType(glyf)" : "未知轮廓"),
                     (unsigned)num_tables, (unsigned)rd_u16(mp + 4));
        }
    }

    /* 按 4 字节对齐（sfnt 表都是 4 字节对齐的），作为 FreeType 的流长度 */
    return (max_end + 3u) & ~3u;
}

/* ---------------- lv_fs 驱动回调（全部只读） ---------------- */

static bool fs_ready_cb(lv_fs_drv_t *drv)
{
    (void)drv;
    return s_part != NULL && s_file_size > 0;
}

static void *fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;

    /* 分区里只有字体这一个文件，而且是只读的 */
    if ((mode & LV_FS_MODE_RD) == 0) {
        ESP_LOGW(TAG, "font 分区只读，拒绝非读模式打开：%s", path);
        return NULL;
    }
    if (strcmp(path, FONT_FILE_NAME) != 0) {
        ESP_LOGW(TAG, "font 分区里没有 %s（只提供 %s）", path, FONT_FILE_NAME);
        return NULL;
    }

    for (int i = 0; i < FONT_OPEN_MAX; i++) {
        if (!s_files[i].in_use) {
            s_files[i].in_use = true;
            s_files[i].pos = 0;
            return &s_files[i];
        }
    }
    ESP_LOGE(TAG, "lv_fs 句柄用尽（%d 个），字体打开失败", FONT_OPEN_MAX);
    return NULL;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    font_file_t *f = (font_file_t *)file_p;
    if (f) f->in_use = false;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    font_file_t *f = (font_file_t *)file_p;

    if (br) *br = 0;
    if (f == NULL) return LV_FS_RES_INV_PARAM;

    /* 读到文件真实末尾就截断：分区里文件之后是 0xFF（擦除态），
       不能让 FreeType 把填充当字形数据处理 */
    if (f->pos >= s_file_size) return LV_FS_RES_OK;
    if (btr > s_file_size - f->pos) btr = s_file_size - f->pos;

    /* ⚠⚠ 优先走 mmap + memcpy（见 font_ttf_init 里的说明）：实测走
       esp_partition_read 会在字形渲染时把系统搞崩 ——
       渲染一个汉字要读 3~6 次，而 esp_flash_read() 在"目标缓冲区不在 DRAM"
       （FreeType 的缓冲都在 PSRAM）且分区**未映射**时，会退化成
       「关外部缓存 + 从 DRAM 临时缓冲中转 + 停住另一核」的重路径
       （见 esp_flash_api.c 的 direct_read / temp_buffer 分支），
       于是每渲染几十个字形就要关/开缓存几百次、反复停另一核。 */
    if (s_map) {
        memcpy(buf, (const uint8_t *)s_map + f->pos, btr);
    } else {
        esp_err_t err = esp_partition_read(s_part, f->pos, buf, btr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_read 失败：%s（offset=%u len=%u）",
                     esp_err_to_name(err), (unsigned)f->pos, (unsigned)btr);
            return LV_FS_RES_HW_ERR;
        }
    }

    f->pos += btr;
    if (br) *br = btr;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    font_file_t *f = (font_file_t *)file_p;
    if (f == NULL) return LV_FS_RES_INV_PARAM;

    switch (whence) {
        case LV_FS_SEEK_SET:
            f->pos = pos;
            break;
        case LV_FS_SEEK_CUR:
            f->pos += pos;
            break;
        case LV_FS_SEEK_END:
            /* FreeType 就是靠 seek(END) + tell 来量文件长度的（pos 传 0） */
            f->pos = s_file_size + pos;
            break;
        default:
            return LV_FS_RES_INV_PARAM;
    }

    if (f->pos > s_file_size) f->pos = s_file_size;   /* 允许定位到"末尾" */
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    font_file_t *f = (font_file_t *)file_p;
    if (f == NULL || pos_p == NULL) return LV_FS_RES_INV_PARAM;
    *pos_p = f->pos;
    return LV_FS_RES_OK;
}

static void register_fs_driver(void)
{
    lv_fs_drv_init(&s_drv);
    s_drv.letter     = FONT_FS_LETTER;
    s_drv.cache_size = FONT_FS_CACHE_SIZE;
    s_drv.ready_cb   = fs_ready_cb;
    s_drv.open_cb    = fs_open_cb;
    s_drv.close_cb   = fs_close_cb;
    s_drv.read_cb    = fs_read_cb;
    s_drv.seek_cb    = fs_seek_cb;
    s_drv.tell_cb    = fs_tell_cb;
    /* write/dir 回调保持 NULL：LVGL 会返回"不支持"，正是我们要的 */
    lv_fs_drv_register(&s_drv);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t font_ttf_init(void)
{
    if (s_ready) return ESP_OK;

    /* 1) 找 font 分区（partitions.csv：font @ 0x800000，4MiB，subtype 0x41） */
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)FONT_PART_SUBTYPE,
                                      FONT_PART_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "找不到 font 分区（检查 partitions.csv），回退内嵌位图字体");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2) 校验并算出字体文件真实长度 */
    s_file_size = probe_font_file();
    if (s_file_size == 0) {
        ESP_LOGE(TAG, "font 分区校验失败，回退内嵌位图字体（重新 `idfc flash` 可修复）");
        s_part = NULL;                 /* 让 ready_cb 返回 false */
        s_file_size = 0;
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "font 分区：offset=0x%05x size=%u B；字体文件实际长度=%u B（%.2f MiB）",
             (unsigned)s_part->address, (unsigned)s_part->size,
             (unsigned)s_file_size, (double)s_file_size / (1024.0 * 1024.0));

    /* 2.5) 把字体分区 mmap 进来，字形读取走 memcpy（关键，勿删）
       ------------------------------------------------------------------
       FreeType 渲染每个字形要读几次文件，而 esp_partition_read() 在我们的
       场景下是**重路径**：目标缓冲区是 FreeType 的 PSRAM 缓冲 → 不在 DRAM →
       esp_flash_read() 必须走「关外部内存缓存 + 申请 DRAM 临时缓冲中转 +
       停住另一核」的流程（esp_flash_api.c 的 direct_read/temp_buffer 分支）。
       字体分区是**未映射**分区，所以每一次读都要这么来一遍：实测拉到界面
       （字形按需光栅化）时会把 Core1 的空闲任务搞崩（PM 锁句柄被写坏）。
       mmap 之后读命中 flash 缓存，不关缓存、不停核、不申请临时缓冲，
       顺带快很多。mmap 失败时退回 esp_partition_read（功能可用但慢且危险，
       会打印警告）。 */
    esp_err_t mmap_err = esp_partition_mmap(s_part, 0, s_part->size,
                                            ESP_PARTITION_MMAP_DATA,
                                            &s_map, &s_map_handle);
    if (mmap_err == ESP_OK) {
        ESP_LOGI(TAG, "font 分区已 mmap 到 %p（%u B），字形读取走 memcpy",
                 s_map, (unsigned)s_part->size);
    } else {
        s_map = NULL;
        ESP_LOGW(TAG, "font 分区 mmap 失败（%s）：退回逐次 esp_partition_read，"
                      "字形渲染会明显变慢且会频繁关缓存/停另一核",
                 esp_err_to_name(mmap_err));
    }

    /* 3) 注册 lv_fs 驱动（此后 "F:/system.ttf" 可被打开） */
    register_fs_driver();

    /* 4) 预创建各字号字体。⚠ 此时 LVGL 任务已在跑，必须持锁再动 LVGL 全局状态；
          创建字体只是登记"用哪个文件、多大字号"，不会立刻光栅化字形。 */
    size_t slot = 0;
    bool   any_ok = false;

    if (lvgl_port_lock(-1)) {
        for (size_t i = 0; i < sizeof(s_preload_px) / sizeof(s_preload_px[0]) && slot < FONT_SLOT_MAX; i++) {
            uint16_t px = s_preload_px[i];
            lv_font_t *font = lv_freetype_font_create(FONT_PATH,
                                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                      px,
                                                      LV_FREETYPE_FONT_STYLE_NORMAL);
            s_slots[slot].px   = px;
            s_slots[slot].font = font;
            slot++;

            if (font) {
                any_ok = true;
                ESP_LOGI(TAG, "TTF 字体就绪：%s %upx（行高 %d，基准线 %d）",
                         FONT_PATH, (unsigned)px,
                         (int)lv_font_get_line_height(font), (int)font->base_line);
            } else {
                ESP_LOGE(TAG, "TTF 字体创建失败（%upx）：文件损坏或不是 TTF/OTF；该字号回退内嵌位图字体", (unsigned)px);
            }
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "取 LVGL 锁失败，字体未创建，回退内嵌位图字体");
        return ESP_ERR_TIMEOUT;
    }

    if (!any_ok) {
        ESP_LOGE(TAG, "所有字号创建失败，界面回退内嵌位图字体");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "系统字体已切换到 TTF（FreeType，字形按需从 font 分区读取）");
    return ESP_OK;
}

const lv_font_t *font_ttf_get(uint16_t px)
{
    if (s_ready) {
        for (size_t i = 0; i < FONT_SLOT_MAX; i++) {
            if (s_slots[i].font && s_slots[i].px == px) {
                return s_slots[i].font;
            }
        }
        /* 没预创建过的字号：不退化成"随便给一个 TTF"（字号不对会立刻撑破布局），
           统一走内嵌位图回退字体，并在日志里提示一次。 */
        static uint16_t warned_px;
        if (warned_px != px) {
            warned_px = px;
            ESP_LOGW(TAG, "字号 %upx 没有预创建，回退内嵌位图字体"
                          "（需要新字号请加进 font_ttf.c 的 s_preload_px）", (unsigned)px);
        }
    }
    return fallback_font_for_px(px);
}

bool font_ttf_is_ready(void)
{
    return s_ready;
}

#endif /* CONFIG_UI_FONT_TTF */
