/**
 * @file lvgl_mem.c
 * @brief LVGL 9 自定义内存分配器：把 LVGL 的对象/样式/绘制缓冲放到 PSRAM
 *
 * ============================== 为什么需要它 ==============================
 * 内部 RAM（DRAM）在这块板子上是本项目最紧张的资源：LVGL 的绘制缓冲必须
 * 落在 DMA 可访问的内部 RAM（见 core/ui/lvgl_port.c），而 app_ui_create()
 * 一次性创建上千个 LVGL 控件对象，实测吃掉约 66KB 内部 RAM —— 迁移到
 * LVGL 9 后内部 RAM 与 DMA 双双见底（实测开机完成时 int 仅剩 23 字节、
 * dma 仅剩 19 字节、最大连续块为 0），直接引发：
 *   · sdmmc_cmd: allocate_dma_buf: not enough mem（SD 读失败 → 误判坏曲）
 *   · newlib 的 fopen 里 lock_init_generic 建互斥量失败 → abort() 崩溃
 *     （cover_load_task 读 .npd 时触发，见 song_meta_load）
 *
 * LVGL 8 无法把对象堆换成外部内存（只有自带静态堆 / 标准 malloc 两种），
 * LVGL 9 正式提供了这一能力：`LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM`
 * （Kconfig: Component config → LVGL → Memory Settings → Malloc functions
 *  source → "Implement the functions externally"），由用户实现下面这组
 * `*_core` 函数。于是本项目把 **LVGL 的所有堆分配**改到 8MB PSRAM，
 * 内部 RAM 则专门留给：LVGL 绘制缓冲（DMA）、各驱动 DMA 缓冲、
 * 实时任务栈。
 *
 * ============================ 安全性说明 ============================
 * 1. **绘制缓冲不受影响**：显示刷屏用的两块缓冲由 lvgl_port.c 用
 *    heap_caps_aligned_alloc(MALLOC_CAP_DMA) 自己分配，不走 lv_malloc，
 *    依旧在内部 DMA RAM；本文件只接管 LVGL 内部对象与中间绘制层。
 * 2. **关缓存窗口安全**：擦写 Flash / pldata mmap-munmap 会临时关掉外部
 *    内存缓存（此时 PSRAM 不可访问）。这类操作全部经 deep_worker 串行执行，
 *    且 IDF 在关缓存时会关中断并停住另一个核，期间 LVGL 任务不会运行，
 *    因此不会读到 PSRAM 里的对象（详见 AGENTS.md「关缓存陷阱」）。
 * 3. **失败回退**：PSRAM 分配失败时退回内部 RAM，保证功能不因内存策略挂掉；
 *    释放/重分配用 IDF 的堆函数，它们能识别指针实际所属的堆区（free() 对
 *    内部 RAM 与 PSRAM 都有效），所以两种来源的指针可以安全混用。
 */

#include "lvgl.h"

#include <string.h>

#include "core/ui/lvgl_mem.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"   /* esp_ptr_internal()：判断指针是否在内部 RAM */
#include "esp_timer.h"          /* esp_timer_get_time()：路径B 的日志节流 */

/* ⚠ 整套实现只在 Kconfig 选中 "Implement the functions externally"
   （CONFIG_LV_USE_CUSTOM_MALLOC）时参与编译：若哪天改回 CLIB，LVGL 自带的
   stdlib/clib/lv_mem_core_clib.c 会提供同名符号，不加守卫就会重复定义。 */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

static const char *TAG = "lvgl_mem";

/* 首选 PSRAM（8MB，充裕）；PSRAM 无 8BIT 能力标记则不可用，一并写上 */
#define LVGL_MEM_PREFER   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
/* 兜底：内部 RAM（8BIT，不要求 DMA —— LVGL 对象不需要 DMA 能力） */
#define LVGL_MEM_FALLBACK (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/* 回退到内部 RAM 只提示一次，避免心跳类小分配把串口刷爆 */
static bool s_fallback_logged = false;

/* ---------------------------------------------------------------------------
   【路径B：热点对象优先内部 RAM】
   实测（列表快速滚动，见 ideas_and_exps/IRAM_AUDIT_LVGL9.md §12）：
     · LVGL 对象在内部 RAM → 21 fps
     · LVGL 对象在 PSRAM   → 11 fps
   对象位置是比"blend 是否进 IRAM"（11 vs 9 fps）大得多的性能杠杆：滚动开销主要
   在对象树的遍历/布局/样式解析，而对象工作集约 88KB，远超本芯片 32KB 的数据 cache。
   但全部对象装不进内部 RAM（§11 实测会直接见底崩），所以改用"按创建时机圈定"：
   只在创建滚动热对象（virtual_list 行槽）的那一小段时间内把分配钉到内部 RAM。

   ⚠ 安全阀：内部 RAM 必须始终给 SD 读(512B)/DMA/内部任务栈留余量，所以窗口内
     也要求空闲量高于 LVGL_INTERNAL_RESERVE 才敢用内部 RAM，不够就自动回退 PSRAM
     （功能不受影响，只是那部分对象不享受加速）。
   --------------------------------------------------------------------------- */
#define LVGL_INTERNAL_RESERVE   (40 * 1024)   /* 窗口内 malloc 的保留底线（按申请字节算） */
/* ⚠ 上面这条保留线是按"申请 size 之和"卡的，而每笔分配还有 ~16B 的堆元数据/对齐
   开销，且 lv_realloc_core 的"继续留在内部 RAM"路径并不受它约束 —— 实测把整个
   app_ui_create 包进窗口后：逻辑上钉了 87,576B，而内部 RAM 窗口结束时实际只剩
   16,483B（稳态 36,467B，最大连续块 7,680B）。所以**不要把 40KB 当成真实余量**，
   真实安全网是下面这条硬底线。 */
#define LVGL_INTERNAL_HARD_FLOOR (12 * 1024)  /* 硬底线：低于它，realloc 不再强留 */

/* 窗口可嵌套（app_ui_create 会把整段 UI 创建包进来，而 virtual_list_create
   内部还会再开一次自己的窗口），所以用深度计数而不是布尔量：
   只有最外层关闭时才复位统计并打印。 */
static int     s_internal_prefer_depth = 0;      /* 窗口嵌套深度（>0 = 生效中） */
static size_t  s_internal_prefer_used  = 0;      /* 本窗口已钉到内部 RAM 的字节数 */
/* ⚠ 初值取负数：保证启动后第一次窗口关闭就能打印统计。若初值为 0，编译进
   "启动后 2 秒内一律不打印"，而行槽恰好在 ~1.3s 创建，统计会被吞掉（实测踩过）。 */
static int64_t s_last_pin_log_us       = -2000000;

static void *lvgl_mem_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* 【路径B】热点对象窗口内：优先内部 RAM（但留足 LVGL_INTERNAL_RESERVE，
       不够就往下走 PSRAM，绝不把内部 RAM 吃穿） */
    if (s_internal_prefer_depth > 0 &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
            LVGL_INTERNAL_RESERVE + size) {
        void *pinned = heap_caps_malloc(size, LVGL_MEM_FALLBACK);
        if (pinned != NULL) {
            s_internal_prefer_used += size;
            return pinned;
        }
    }

    void *p = heap_caps_malloc(size, LVGL_MEM_PREFER);
    if (p == NULL) {
        p = heap_caps_malloc(size, LVGL_MEM_FALLBACK);
        if (p != NULL && !s_fallback_logged) {
            s_fallback_logged = true;
            ESP_LOGW(TAG, "PSRAM alloc failed (size=%u), fallback to internal RAM",
                     (unsigned)size);
        }
    }
    return p;
}

/* lv_init() 会调用；CUSTOM 模式下没有自带的 core 文件，必须自己提供 */
void lv_mem_init(void)
{
    return;   /* 无需初始化：直接用 IDF 堆 */
}

void lv_mem_deinit(void)
{
    return;   /* 无需释放：每个分配都由 lv_free 单独归还 */
}

/* 内存池接口（LVGL 的"往自有堆里加一块内存"功能）：CUSTOM 模式不支持 */
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
    return;
}

/* lv_malloc() 的底层实现 */
void *lv_malloc_core(size_t size)
{
    return lvgl_mem_alloc(size);
}

/* lv_free() 的底层实现：heap_caps_free 按指针所属堆区释放，内部/PSRAM 通用 */
void lv_free_core(void *p)
{
    heap_caps_free(p);
}

/* lv_realloc() 的底层实现。
   ⚠ heap_caps_realloc 在"指针所属堆区与新 caps 不匹配"时会自动
   malloc + memcpy + free（IDF 的 heap_caps_realloc_base 行为），
   失败时原指针仍然有效，符合 lv_realloc 的约定。 */
void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == NULL) {
        return lvgl_mem_alloc(new_size);
    }
    if (new_size == 0) {
        heap_caps_free(p);
        return NULL;
    }

    /* 【路径B】原指针若已被钉在内部 RAM（热点窗口内分配的），优先让它继续留在
       内部 RAM：否则 heap_caps_realloc 会因为 caps 不匹配而 malloc+memcpy+free，
       把对象"赶"回 PSRAM，钉住就白做了（文本缓冲每次改文本都会走 realloc，
       这一条是必须的）。
       ⚠ 窗口关闭后这条路径依然生效（滚动改文本时不能让对象被搬走），所以这里
       不能拿 LVGL_INTERNAL_RESERVE 去卡 —— 改为只守一条硬底线：把原分配的占用
       算回来之后，仍需高于 LVGL_INTERNAL_HARD_FLOOR，确保极端情况下吃不穿。 */
    if (esp_ptr_internal(p) &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) +
            heap_caps_get_allocated_size(p) >= LVGL_INTERNAL_HARD_FLOOR + new_size) {
        void *np = heap_caps_realloc(p, new_size, LVGL_MEM_FALLBACK);
        if (np != NULL) {
            return np;
        }
    }

    void *np = heap_caps_realloc(p, new_size, LVGL_MEM_PREFER);
    if (np == NULL) {
        np = heap_caps_realloc(p, new_size, LVGL_MEM_FALLBACK);
    }
    return np;
}

/* lv_mem_monitor() 的底层实现（LV_USE_MEM_MONITOR 打开时显示用）：
   汇报 PSRAM 堆（LVGL 分配的主战场）的状态 */
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, LVGL_MEM_PREFER);

    size_t total = info.total_free_bytes + info.total_allocated_bytes;

    mon_p->total_size = total;
    mon_p->free_cnt = info.free_blocks;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = info.total_allocated_bytes;
    mon_p->used_pct = (total > 0)
        ? (uint8_t)((info.total_allocated_bytes * 100) / total)
        : 0;

    /* 碎片率：1 - 最大连续块 / 总空闲（越接近 100 越碎） */
    if (info.total_free_bytes > 0) {
        size_t frag = 100 - (info.largest_free_block * 100) / info.total_free_bytes;
        mon_p->frag_pct = (uint8_t)frag;
    } else {
        mon_p->frag_pct = 0;
    }
}

/* lv_mem_test() 的底层实现：分配一次再释放，能跑通即认为堆可用 */
lv_result_t lv_mem_test_core(void)
{
    void *p = lvgl_mem_alloc(1);
    if (p == NULL) {
        return LV_RESULT_INVALID;
    }
    heap_caps_free(p);
    return LV_RESULT_OK;
}

/* ---------------------------------------------------------------------------
   路径B 的窗口开关：进入/离开"热点对象"窗口（说明见文件上方与 lvgl_mem.h）。
   ⚠ 必须配对调用；窗口内不要跨任务/跨锁。当前唯一调用点是 virtual_list 的行槽创建。
   --------------------------------------------------------------------------- */
void lvgl_mem_set_internal_prefer(bool on)
{
    if (on) {
        if (s_internal_prefer_depth++ == 0) {
            s_internal_prefer_used = 0;   /* 只有最外层进入时才复位统计 */
        }
        return;
    }

    if (s_internal_prefer_depth == 0) {
        return;   /* 防御：未配对的关闭，直接忽略 */
    }
    if (--s_internal_prefer_depth > 0) {
        return;   /* 还有外层窗口没结束 */
    }

    /* 最外层窗口结束：打印本次钉住量。
       绑定窗口在滚动时会高频开关，日志节流到每 2s 一条，避免刷屏。 */
    int64_t now = esp_timer_get_time();
    if (s_internal_prefer_used > 0 && (now - s_last_pin_log_us) > 2000000) {
        s_last_pin_log_us = now;
        ESP_LOGI(TAG, "hot objects pinned to internal RAM: %u B in this window (int free now %u B)",
                 (unsigned)s_internal_prefer_used,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
}

#else  /* LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM：LVGL 用自带分配器，本开关无效 */

void lvgl_mem_set_internal_prefer(bool on)
{
    LV_UNUSED(on);
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
