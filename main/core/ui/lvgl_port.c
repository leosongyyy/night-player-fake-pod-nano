#include "core/ui/lvgl_port.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "core/ui/touch_test.h"
#include "core/ui/lvgl_ppa_accel.h"
#include "core/ui/lvgl_ppa_rotate.h"

#include <assert.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;
static TaskHandle_t s_lvgl_task = NULL;   /* LVGL 任务句柄（供息屏/深度休眠前挂起） */
/* LVGL 时间来源：见下方 lvgl_port_tick_cb —— 直接读硬件时间（esp_timer_get_time），
   **不再**用 esp_timer 定时器喂 lv_tick_inc（那正是"整屏定格"的根因，
   见 ideas_and_exps/DISPLAY_FREEZE_TRIAGE.md）。下面两个变量实现息屏时的"冻结"：
     s_tick_pause_start_us  ≥0 表示时间已冻结，值 = 冻结起点
     s_tick_paused_total_us 累计被冻结的时长（恢复后从暂停点继续，不补跑） */
static volatile int64_t s_tick_pause_start_us  = -1;
static volatile int64_t s_tick_paused_total_us = 0;
/* ---- LVGL 9 对象（v8 的 disp_drv/indev_drv/draw_buf 静态结构体已改为不透明句柄）----
   v9 的显示/输入设备由 lv_display_create() / lv_indev_create() 动态创建，
   这里保存句柄；绘制缓冲仍是自管的两块 DMA 内存（见 lvgl_port_init）。 */
static lv_display_t *s_disp = NULL;   /* LVGL 显示对象（flush/rounder 回调挂在其上） */
static lv_indev_t *s_indev = NULL;    /* 触摸输入设备对象（供 LVGL 控件接收触摸事件） */
static uint8_t *s_draw_buf1 = NULL;   /* 绘制缓冲 1（DMA，见 EXAMPLE_LVGL_BUF_HEIGHT） */
static uint8_t *s_draw_buf2 = NULL;   /* 绘制缓冲 2（DMA，双缓冲） */
/* 实际采用的单块缓冲字节数：正常是 V_RES/10，DMA 连续块不够时会降档（/12、/16）。
   PPA 旋转 tile 也按这个尺寸分配，所以必须把"最终值"存下来给
   lvgl_ppa_rotate_init() 用，不能再用编译期常量。 */
static size_t s_draw_buf_bytes = 0;

/* 刷新帧计数：LVGL 每完成一次屏幕刷新（LV_EVENT_REFR_READY）加一。
   用途：LVGL 自带的 FPS 监视器只把数字画在屏幕角落，串口看不到；真机抓
   "滚动时多少帧"、给对象放内部 RAM / PSRAM 做 A/B 时需要从串口读，
   由 performance_monitor 每 2s 取一次差值换算 fps=。
   ⚠ 只统计"真的发生刷新"的帧，静止时计数不增长 → fps 显示 0，属正常。 */
static volatile uint32_t s_refr_cnt = 0;

static void lvgl_refr_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY) {
        s_refr_cnt++;
    }
}

uint32_t lvgl_port_get_refr_count(void)
{
    return s_refr_cnt;
}

/* 触摸句柄（触摸异步初始化，就绪前为 NULL）：由触摸初始化完成回调
   （lvgl_port_set_touch_handle）写入，read_cb 读取；就绪前不给触摸数据 */
static esp_lcd_touch_handle_t s_touch_handle = NULL;

/* 绘制缓冲高度：屏幕高度的 1/10（460/10 = 46 行，LVGL 内部再对齐到偶数）。
   每块 = 460 × 46 × 2B ≈ 41KB，双缓冲合计约 83KB **DMA 内存**
   （这也是 LVGL 官方建议的最小值：≥ 1/10 屏）。
   ⚠ 历史：/10 曾把开机完成后的 DMA 池压到不足 3KB，SD 卡读操作的 DMA
   分配失败（`sdmmc_cmd: allocate_dma_buf: not enough mem`）→ 切歌被误判
   "坏曲"自动跳过，因此一度降到 /12（双缓冲 ≈68KB，DMA 稳态余量约 15KB）；
   再降到 /20 帧率会掉到约 20fps。
   2026-09-10 按用户要求改回 /10 做一轮实测（用更大缓冲换帧率）：
   重点盯 `heap_caps_get_free_size(MALLOC_CAP_DMA)`、FPS 与 SD 读失败日志，
   若再现 allocate_dma_buf 失败/无故跳曲，退回 /12。
   2026-11 经典蓝牙接入后改成 /16：BT 控制器 + 协议栈从内部 DMA 池吃掉
   约 13KB（实测 /10 时开机完成后 DMA 仅剩 31B，SD 读全部失败 → 所有歌被判
   "坏曲"）。/16 每块 460×28×2B≈25KB、双缓冲≈51KB，比 /10 省约 33KB DMA；
   代价是帧率略降（/20 约 20fps、/10 约 30fps，/16 约 25fps 量级）。
   【2026-11-15】WiFi对时从播放器里摘出去（改双固件方案：网络栈搬进 ota_0 的
   对时固件，播放器这边省 ~27KB 内部 RAM，见 ideas_and_exps/DUAL_APP_TIME_SYNC.md）
   + LVGL/main 任务栈回收后改回 /10，目标是把帧率恢复到加蓝牙前的水平；
   同时把分配改成**逐级回退**（/10 → /12 → /16），万一连续块不够也不会 assert
   挂启动，只会降档并打印实际档位。 回退档位见下方 k_buf_divs。 */
#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES / 10)
/* ⚠ 本移植层已没有"tick 周期"这个概念：tick 由 lv_tick_set_cb() 直接读硬件时间，
   原来是"esp_timer 每 2 ms 回调一次 lv_tick_inc(2)"，那套正是整屏定格的根因
   （见 lvgl_port_tick_cb 上方注释）。保留这行注释只为提醒后来者别再引回来。 */
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
/* ⚠ 栈 12KB（LVGL 9 迁移时 6KB→12KB；2026-10 TTF 迁移 12→20KB；
   **2026-11 改回位图字体后 20→12KB**，2026-11-15 再次确认）：
   栈里要跑 lv_timer_handler 的整条渲染链（控件事件回调 → 布局/重绘 →
   各 draw unit → 刷屏 esp_lcd_panel_draw_bitmap 的 QSPI DMA 链）。
   LVGL 8.4 下实测高水位约 3.3KB，6KB 足够；LVGL 9 的绘制管线明显更深
   （draw task / layer / 混合函数层层下探），6KB 会在"首次渲染设置页"
   这种重页面时溢出 —— 实测现象：从主界面滑到第 4 页（播放设置）必定
   `A stack overflow in task LVGL has been detected` 并 panic 重启。
   改到 12KB 后位图字体时代不再溢出（当年实测高水位 5532B，余量 ~6.5KB）。
   2026-10 TTF 迁移时 12KB 不够（FreeType 光栅化池在栈上，实测只剩 172B），
   所以曾上调到 20KB；**2026-11 改回 NPF1 位图字体后不再光栅化**，20KB 的
   额外 8KB 纯属 TTF 遗留 —— 2026-11-15 按用户要求回收（内部 RAM 换帧率）。
   ⚠ 恢复 12KB 后必须回归："首次进设置页"、"息屏→亮屏全屏重刷"
   （deep_worker 里 lv_refr_now 全屏光栅化是最深的路径）、长列表滚动。
   若出现 `stack overflow in task LVGL`，改回 16KB 再测。
   ⚠ 内部栈不能走 PSRAM（见 AGENTS.md），加栈会吃内部 RAM，改动前先看
   mem_monitor 的 stk 行与内部 RAM 余量。 */
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (12 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

/* ESP-IDF 面板 IO 的"传输完成"回调（中断上下文）：
   通知 LVGL 这一块绘制缓冲已经送完，可以复用。
   v8 时代这里拿的是 lv_disp_drv_t*（disp_drv 是静态对象，display_init 早于
   lvgl_port_init 也能取地址）；v9 的 lv_display_t 是运行时创建的，因此改为
   由 display_init 传入 &s_disp（见 lvgl_port_get_flush_ctx 的说明），
   这样即使回调早于创建触发也能安全判空。
   ⚠ 这里曾经还统计"提交→传输完成的耗时"并在 lvgl_port_task 里每 2s 打印一次
     占空比（`flush 2s: … 占空 N%`）。那套统计已删除（2026-11 用户确认不再使用）：
     结论早就拿到了——40MHz QSPI ≈ 16MB/s，全屏 423KB 单帧传输就 20ms 以上，
     占空比必然贴着 100%，带宽天花板是已知常量，不需要常驻计数器。 */
bool lvgl_port_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t **disp = (lv_display_t **)user_ctx;
    if (disp != NULL && *disp != NULL) {
        lv_display_flush_ready(*disp);
    }
    return false;
}

/* 刷屏提交的失败记账：esp_lcd_panel_draw_bitmap() 返回 ESP_ERR_* 时**不能**再
   静默吞掉（原实现完全忽略返回值）—— 屏类问题（QSPI/DMA/面板状态）的第一个
   可信信号就是它，否则只能看到"画面不动"却查不出是谁拒绝了传输。
   ⚠ 刷屏是热路径（滚动时约 500 次/秒），失败通常成串出现，所以只在
   前 3 次与每 256 次打一条日志，避免日志本身把帧率压垮。
   （曾经还有一对 lvgl_port_flush_error_count / _stats_reset 供上层取数，
     全项目零调用，2026-11 随刷屏耗时统计一起删除。） */
static volatile uint32_t s_flush_err_cnt = 0;   /* 累计失败次数 */

/* v9 的 flush 回调签名：像素数据是 uint8_t*（不再有 lv_color_t*），
   显示对象由参数传入（v8 时靠 drv->user_data 取面板句柄）。 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) lv_display_get_user_data(disp);
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

#if CONFIG_UI_DISPLAY_ROTATE_PPA
    /* 实验性：整体顺时针旋转（PPA 硬件 SRM，见 core/ui/lvgl_ppa_rotate.c）。
       成功时本块已完成「旋转 + 字节序交换 + 按旋转后坐标提交」，直接返回；
       失败（未初始化/角度为 0/PPA 出错）则往下走原路径，保证界面仍然可用。
       ⚠ 触摸坐标由本文件的 touchpad read_cb 按同一角度做逆变换，二者必须
         保持一致（角度都取 lvgl_ppa_rotate_get_angle() 的"实际生效值"）。 */
    if (lvgl_ppa_rotate_flush(area, px_map)) {
        return;
    }
#endif

    /* ⚠ 字节序：v8 靠 LV_COLOR_16_SWAP=y 让整条渲染管线直接产出"高字节在前"的
       RGB565；v9 移除了该配置项，官方做法是在 flush 回调里把这一块缓冲整体
       交换一次（见 LVGL 9.4 文档 "Display / Color Format / Swapping Endian-ness"）。
       代价是每次刷屏多一遍 ~2 字节/像素的遍历（460×46 ≈ 2.1 万像素，远小于
       33ms 预算），换来渲染格式保持默认的 RGB565（LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
       在 ESP-IDF 的 Kconfig 里没有开关、恒为 0，无法直接把 RGB565_SWAPPED 当
       目标格式用）。 */
#if LCD_BIT_PER_PIXEL == 16
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)lv_area_get_size(area));
#endif

#if LCD_BIT_PER_PIXEL == 24
    /* RGB888：面板期望 R,G,B 顺序，而 LVGL 的 RGB888 缓冲按 B,G,R 存放，
       这里就地交换每像素的首尾字节（从后往前写，避免覆盖还没读的像素）。
       ⚠ 本项目实际固定 16bpp（见 display.h 的 LCD_BIT_PER_PIXEL），此分支
       仅为保留 24bpp 屏幕配置的可能，未经实测。 */
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);
    for (int i = pixel_num - 1; i >= 0; i--) {
        uint8_t temp = px_map[i * 3 + 0];
        px_map[i * 3 + 0] = px_map[i * 3 + 2];
        px_map[i * 3 + 2] = temp;
    }
#endif

    // copy a buffer's content to a specific area of the display
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1,
                                                    offsetx2 + 1, offsety2 + 1, px_map);
    if (err != ESP_OK) {
        const uint32_t n = ++s_flush_err_cnt;
        if (n <= 3 || (n % 256) == 0) {
            ESP_LOGE(TAG, "panel draw_bitmap failed: %s (area %dx%d+%d+%d, total %u)",
                     esp_err_to_name(err), (int)(offsetx2 - offsetx1 + 1),
                     (int)(offsety2 - offsety1 + 1), (int)offsetx1, (int)offsety1,
                     (unsigned)n);
        }
    }
}

/* 重绘区域对齐回调：v9 不再有 disp_drv.rounder_cb，改为给显示对象挂
   LV_EVENT_INVALIDATE_AREA 事件（参数即待重绘区域 lv_area_t*）。 */
static void lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area == NULL) {
        return;
    }

    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

/* ====================================================================
 * LVGL tick：直接取硬件时间，**不用 esp_timer 定时器喂 lv_tick_inc**
 *
 * ⚠⚠ 2026-11 真实事故（整屏定格、日志无异常，详见
 *    ideas_and_exps/DISPLAY_FREEZE_TRIAGE.md）：
 * 原实现是"esp_timer 每 2 ms 回调一次 → lv_tick_inc(2)"。而 IDF 6.1 的
 * esp_timer 把定时器拆成**两条独立的 systimer alarm 通道**
 * （`timestamp_id[0]`=任务派发、`[1]`=中断派发，见
 * `esp_timer_impl_common.c`），混合使用时它靠比较两个 timestamp 来猜
 * "这次中断是哪条通道触发的"，猜错就把**任务派发那条通道的闹钟时间**
 * 清成 UINT64_MAX —— 此后任务派发的定时器**永远不再到期**：
 *   · tick 回调计数归零、`esp_timer_is_active()` 仍返回 1、
 *     `esp_timer_get_next_alarm()` 显示闹钟时间早已过去；
 *   · 而本项目的 performance_monitor 正有一个 `ESP_TIMER_ISR`
 *     （中断派发）的 500 ms 采样定时器 —— 两个通道同时存在，条件齐备。
 * 后果：LVGL 的 tick 停 ⇒ 所有 lv_timer 一起停 ⇒ **整屏定格**，
 * 触摸/按键/音频全正常（它们不依赖 LVGL tick），息屏再亮屏
 * （走 lv_refr_now 全屏重刷）才"看起来恢复"。
 *
 * 修复：tick 改由 LVGL 9 的 `lv_tick_set_cb()` 提供，时间直接来自
 * systimer 计数（`esp_timer_get_time()`）——**一个 esp_timer 都不用**，
 * 任务派发路径坏掉也与 LVGL 无关，还顺带省掉 500 次/秒的中断开销。
 *
 * ⚠ `lv_tick_set_cb()` **必须在 `lv_init()` 之后调用**：`lv_init()` 里
 *   `lv_global_init()` 会 `lv_memzero(global, sizeof(lv_global_t))`，
 *   把 tick_get_cb 一起清零。写在前面 → `lv_tick_get()` 恒为 0 →
 *   界面只在 lv_refr_now（解锁/亮屏）时刷一帧（实测踩过）。
 * ==================================================================== */
static uint32_t lvgl_port_tick_cb(void)
{
    /* 冻结期间（息屏挂起）返回挂起那一刻的时间：LVGL 的所有 timer/动画
       从暂停点继续，不会在恢复后一次性补跑挂起期间到期的 timer。 */
    const int64_t now_us = (s_tick_pause_start_us >= 0)
                           ? s_tick_pause_start_us
                           : esp_timer_get_time();
    /* 减去累计冻结时长 → 恢复后时间轴连续（uint32 毫秒，约 49.7 天回绕，
       LVGL 内部按无符号差值比较，回绕安全） */
    return (uint32_t)((now_us - s_tick_paused_total_us) / 1000);
}

bool lvgl_port_lock(int timeout_ms)
{
    assert(lvgl_mux && "lvgl_port_init must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    assert(lvgl_mux && "lvgl_port_init must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (lvgl_port_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            // Release the mutex
            lvgl_port_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/* 供 LCD 面板 IO 作为回调上下文使用的稳定地址：回调触发时解引用即得到当前
   显示对象。v9 的 lv_display_t 要等 lvgl_port_init 才创建，而 display_init
   （早于它）就要把上下文交给 esp_lcd，因此这里交出的是"保存显示对象指针的
   变量地址"，而不是显示对象本身。 */
void *lvgl_port_get_flush_ctx(void)
{
    return (void *)&s_disp;
}

/* 触摸事件订阅（多订阅者）：由 indev 触摸轮询统一驱动，避免各功能重复轮询。
   固定数组容量：触摸测试 + 息屏活动检测 + 预留，够用。 */
#define TOUCH_CB_MAX  4

typedef struct {
    lvgl_port_touch_cb_t cb;
    void *ctx;
} touch_cb_entry_t;

static touch_cb_entry_t s_touch_cbs[TOUCH_CB_MAX];
static uint8_t s_touch_cb_count = 0;

/* 触摸输入设备读取回调：把 esp_lcd_touch 的坐标/按下状态交给 LVGL，
   并转发给所有注册的触摸事件订阅者（如触摸测试、息屏活动检测）。 */

/* ====================================================================
 * 触摸坐标变换（旋转 + 左右手校准；顺序不可颠倒）
 *
 * ① 先按**画面旋转角度**做逆变换（面板坐标 → LVGL 逻辑坐标）：
 *    画面顺时针转 θ 后，逻辑点 p 会显示在面板的 R_θ(p)；手指给的是面板坐标，
 *    所以要反过来转 θ 才能命中同一个控件。例：θ=90° 时逻辑左上角显示在面板
 *    右上角，于是面板 (459,0) 必须变回逻辑 (0,0)。
 *    角度取"实际生效"值（lvgl_ppa_rotate_get_angle()）：PPA 初始化失败时
 *    画面其实没转，这里也必须按 0 处理，否则触摸会和画面对不上。
 *
 * ② 再做 ×29/30 缩放校准，**锚角由握持手型决定**（2026-11 对该款触摸屏的
 *    结论）：这块屏的 29/30 系数本来只是"右撇子 + 不旋转"握法下的经验值，
 *    所以：
 *      · 右撇子（默认）：以**旋转后的左上角**为原点 —— x、y 都向 0 收；
 *      · 左撇子：以**旋转后的右上角**为原点 —— x 向右边收、y 仍从顶部收。
 *    只有"右撇子 + 不旋转"这一种组合才与加的旋转功能之前的旧代码等价。
 *
 * 为什么放在 LVGL 移植层而不是 touch.c 的 process_coordinates：
 * 这里是**唯一**的坐标出口——LVGL 的 indev 数据源和所有触摸订阅者
 * （触摸测试白点、息屏活动检测、ClickWheel）都拿这一份值，变换放这里
 * 才能保证"白点看到的点"和"控件命中的点"是同一个点。
 *
 * ⚠ 想关闭缩放校准：把 TOUCH_CAL_NUM/DEN 改成 30/30（旋转仍然生效）。
 * ==================================================================== */
#define TOUCH_CAL_NUM   29
#define TOUCH_CAL_DEN   30
#if (TOUCH_CAL_DEN) == 0
#error "TOUCH_CAL_DEN must not be 0"
#endif

/* 最后一次"有效按下"的坐标。
   ⚠ 抬手那一帧必须继续上报这个坐标，不能报 (0,0)——历史 bug（LVGL 9 迁移后
   进度条 seek 恒为 0）：LVGL 的 lv_slider_event 在处理 LV_EVENT_RELEASED /
   LV_EVENT_PRESS_LOST 时会调 update_knob_pos()，用 lv_indev_get_point() 把滑块
   值按"当前触摸点"重算一遍。若驱动在抬手帧上报 (0,0)，滑块值就会被重算成
   min（0）并 clamp 到最左，等我们注册的 RELEASED 回调执行时
   lv_slider_get_value() 早已变成 0 → 点击轨道/拖动松手都 seek 到 0 秒。
   （LVGL 8.4 的 RELEASED 分支不重算值，所以升级前没暴露这个问题。）
   真实触摸控制器在抬手时本来就保留最后坐标，这里按同样语义上报即可：
   任何"在 RELEASED 里读坐标/读控件值"的控件都会因此恢复正常。 */
static uint16_t s_last_touch_x = 0;
static uint16_t s_last_touch_y = 0;

static void lvgl_touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    /* 触摸控制器异步初始化中（句柄未就绪）：不给数据，保持未按下，
       避免操作无效指针；就绪后（lvgl_port_set_touch_handle）自动恢复 */
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = s_last_touch_x;
    data->point.y = s_last_touch_y;
    if (s_touch_handle == NULL) {
        return;
    }

    esp_lcd_touch_handle_t tp = s_touch_handle;
    uint16_t touchpad_x = 0, touchpad_y = 0;
    uint8_t touchpad_cnt = 0;

    esp_lcd_touch_read_data(tp);
    /* ⚠ esp_lcd_touch_get_coordinates() 已被 esp_lcd_touch 标记弃用（2.0.0 移除），
       改用新的 esp_lcd_touch_get_data()：错误码用 esp_err_t 返回，坐标放在
       esp_lcd_touch_point_data_t 数组里。本板是单点触摸，取 coords[0] 即可。 */
    esp_lcd_touch_point_data_t tp_data[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    bool touched = false;
    if (esp_lcd_touch_get_data(tp, tp_data, &touchpad_cnt,
                               CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK &&
        touchpad_cnt > 0) {
        touchpad_x = tp_data[0].x;
        touchpad_y = tp_data[0].y;
        touched = true;
    }

    /* ① 先按画面旋转做逆变换，② 再按左右手做 29/30 缩放（顺序不可颠倒，
       推导与"为什么是逆变换"见 TOUCH_CAL_NUM 上方的长注释）。
       ⚠ 只有真正按下（读到有效坐标）时才更新"最后坐标"基准；抬手帧沿用
       上一次按下的坐标（见上方 s_last_touch_* 注释）。
       ⚠ 用 uint32_t 中转再转回 uint16_t：459×29 = 13311 不会溢出，但保留
       宽中间量以免将来调大分子时静默截断。 */
    if (touched) {
        const uint16_t rot = lvgl_ppa_rotate_get_angle();   /* 实际生效角度（未启用/失败=0） */
        const uint32_t max_x = EXAMPLE_LCD_H_RES - 1;
        const uint32_t max_y = EXAMPLE_LCD_V_RES - 1;
        /* 先钳到面板坐标范围内：旋转要用 max_x/max_y 做减法，原始坐标越界
           （控制器噪声/校准异常）会下溢成巨大值，转回 uint16 后乱跳 */
        if (touchpad_x > max_x) {
            touchpad_x = (uint16_t)max_x;
        }
        if (touchpad_y > max_y) {
            touchpad_y = (uint16_t)max_y;
        }
        uint32_t tx = touchpad_x;
        uint32_t ty = touchpad_y;

        switch (rot) {
        case 90: {                       /* 逆时针 90°：面板 (x,y) → 逻辑 (y, H-1-x) */
            const uint32_t x = tx;
            tx = ty;
            ty = max_y - x;
            break;
        }
        case 180:                        /* 中心对称 */
            tx = max_x - tx;
            ty = max_y - ty;
            break;
        case 270: {                      /* 顺时针 90°：面板 (x,y) → 逻辑 (W-1-y, x) */
            const uint32_t y = ty;
            tx = max_x - y;
            ty = touchpad_x;
            break;
        }
        default:                         /* 0°：不变 */
            break;
        }

        if (config_store_get_touch_left_handed()) {
            /* 左撇子：以旋转后右上角 (max_x, 0) 为原点 */
            tx = max_x - ((max_x - tx) * TOUCH_CAL_NUM) / TOUCH_CAL_DEN;
        } else {
            /* 右撇子：以旋转后左上角 (0, 0) 为原点 */
            tx = (tx * TOUCH_CAL_NUM) / TOUCH_CAL_DEN;
        }
        ty = (ty * TOUCH_CAL_NUM) / TOUCH_CAL_DEN;

        s_last_touch_x = (uint16_t)tx;
        s_last_touch_y = (uint16_t)ty;
    }
    touchpad_x = s_last_touch_x;
    touchpad_y = s_last_touch_y;

    data->state = touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touchpad_x;
    data->point.y = touchpad_y;

    for (uint8_t i = 0; i < s_touch_cb_count; i++) {
        if (s_touch_cbs[i].cb != NULL) {
            s_touch_cbs[i].cb(touched, touchpad_x, touchpad_y, s_touch_cbs[i].ctx);
        }
    }
}

void lvgl_port_register_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx)
{
    if (cb == NULL || s_touch_cb_count >= TOUCH_CB_MAX) {
        return;
    }
    /* 相同回调+上下文重复注册：忽略，避免重复通知 */
    for (uint8_t i = 0; i < s_touch_cb_count; i++) {
        if (s_touch_cbs[i].cb == cb && s_touch_cbs[i].ctx == user_ctx) {
            return;
        }
    }
    s_touch_cbs[s_touch_cb_count].cb = cb;
    s_touch_cbs[s_touch_cb_count].ctx = user_ctx;
    s_touch_cb_count++;
}

void lvgl_port_unregister_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return;
    }
    for (uint8_t i = 0; i < s_touch_cb_count; i++) {
        if (s_touch_cbs[i].cb == cb && s_touch_cbs[i].ctx == user_ctx) {
            /* 用最后一个元素填补空位，保持数组紧凑 */
            s_touch_cbs[i] = s_touch_cbs[s_touch_cb_count - 1];
            s_touch_cb_count--;
            return;
        }
    }
}

/* 设置触摸句柄：触摸异步初始化完成后由上层（main.c 的 ready 回调）调用，
   read_cb 在句柄就绪前返回"未按下"，就绪后自动恢复触摸输入 */
void lvgl_port_set_touch_handle(esp_lcd_touch_handle_t touch_handle)
{
    s_touch_handle = touch_handle;
}

void lvgl_port_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch_handle)
{
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    /* tick 必须**在 lv_init() 之后**注册（lv_init 会清零 lv_global_t，
       见 lvgl_port_tick_cb 上方注释）。 */
    lv_tick_set_cb(lvgl_port_tick_cb);

    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    /* ⚠ 必须 MALLOC_CAP_DMA：SPI 主机发送时若缓冲不是 DMA 能力，会为每次传输
       分配一块 DMA "bounce" 缓冲并拷贝（setup_dma_priv_buffer），而 DMA 内存池
       本就很小——曾实测分配失败 → panel_io_spi_tx_color 失败 → 刷屏阻塞 → 任务
       看门狗复位。缓冲高度见 EXAMPLE_LVGL_BUF_HEIGHT 注释（当前 V_RES/10：
       460×46×2B ≈ 41KB/块，双缓冲共约 83KB）；该值直接决定留给 SD 卡读操作的
       DMA 余量（sdmmc_cmd: allocate_dma_buf: not enough mem 曾导致"切歌随机
       报坏曲"）；调这个值必须同时看帧率与 heap_caps_get_free_size(MALLOC_CAP_DMA)。 */
    /* v9 的绘制缓冲是裸内存块（uint8_t*），大小按字节传给 lv_display_set_buffers；
       LVGL 内部会就地包装成 lv_draw_buf_t（partial 模式下高度 = 缓冲字节数 / 行字节数）。
       ⚠ 必须满足 lv_draw_buf_align 的对齐要求（LV_DRAW_BUF_ALIGN，ESP-IDF 下为 4），
       否则 lv_display_set_buffers 里的断言会直接挂掉——用 heap_caps_aligned_alloc。
       ⚠⚠ 每像素字节数**必须显式取 RGB565 = 2**，不能再用 v8 的
       `sizeof(lv_color_t)`：v9 的 lv_color_t 恒为 RGB888（3 字节，与
       LV_COLOR_DEPTH 无关），照抄会多要 50% 的 DMA —— 实测直接导致第二块
       缓冲分配失败（assert s_draw_buf2 挂在启动阶段）。 */
    /* ⚠ 缓冲字节数向上取整到 64B：PPA（CONFIG_UI_PPA_ACCEL）要求 out.buffer 与
       out.buffer_size 都按 cache line 对齐，否则驱动内部 msync 会越过缓冲尾部。
       多出来的 <64 字节不会被 LVGL 当成一行（h = buf_size / stride 向下取整）。
       ⚠ 分配**逐级回退**：先要 /10（加蓝牙前的帧率配置），DMA 连续块不够时
       自动降 /12、再降 /16，只打日志不 assert —— 两缓冲 + PPA 旋转 tile 一共
       要 3 块这么大的 DMA，开机时连续块不够的概率不为零，宁可帧率降档也不能
       让整机起不来。 */
    static const uint8_t k_buf_divs[] = { 10, 12, 16 };
    for (size_t i = 0; i < sizeof(k_buf_divs) / sizeof(k_buf_divs[0]); i++) {
        const size_t bytes =
            ((size_t)EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES / k_buf_divs[i])
             * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) + 63u)
            & ~(size_t)63u;
        s_draw_buf1 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes,
                                              MALLOC_CAP_DMA);
        if (s_draw_buf1 == NULL) {
            ESP_LOGW(TAG, "draw buf1 分配失败（V_RES/%u，%u B），降档重试",
                     (unsigned)k_buf_divs[i], (unsigned)bytes);
            continue;
        }
        s_draw_buf2 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes,
                                              MALLOC_CAP_DMA);
        if (s_draw_buf2 == NULL) {
            ESP_LOGW(TAG, "draw buf2 分配失败（V_RES/%u），降档重试",
                     (unsigned)k_buf_divs[i]);
            heap_caps_free(s_draw_buf1);
            s_draw_buf1 = NULL;
            continue;
        }
        s_draw_buf_bytes = bytes;
        ESP_LOGI(TAG, "draw buffers: V_RES/%u (%u B/块, 双缓冲 %u B, "
                      "DMA 余量 %u B, 最大连续块 %u B)",
                 (unsigned)k_buf_divs[i], (unsigned)bytes,
                 (unsigned)(bytes * 2),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        break;
    }
    assert(s_draw_buf1 != NULL && s_draw_buf2 != NULL && s_draw_buf_bytes != 0);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    /* v9：显示对象在 lv_init 之后动态创建，回调与缓冲都挂到该对象上 */
    s_disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    assert(s_disp);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_draw_buf1, s_draw_buf2, s_draw_buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_disp, panel_handle);
    /* 重绘区域对齐：v8 的 disp_drv.rounder_cb 在 v9 里改为显示事件 */
    lv_display_add_event_cb(s_disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    /* 刷新帧计数（供串口性能监视换算 fps，见 s_refr_cnt 注释） */
    lv_display_add_event_cb(s_disp, lvgl_refr_event_cb, LV_EVENT_REFR_READY, NULL);

    /* PPA 混合加速（CONFIG_UI_PPA_ACCEL）：注册自定义 SW blend handler。
       必须在显示对象与绘制缓冲就绪之后、LVGL 任务开始渲染之前调用。
       只接管"向 RGB565 图层做图像混合"的操作，其余仍走 CPU。 */
    lvgl_ppa_accel_init(s_disp);
    /* 性能自测（仅 CONFIG_UI_PPA_BENCH 开启时有效，正式固件是空函数） */
    lvgl_ppa_accel_benchmark();

    /* 实验性画面旋转（CONFIG_UI_DISPLAY_ROTATE_PPA）：在 flush 回调里用 PPA 的
       SRM 把每块绘制缓冲按设定角度旋转再提交面板（触摸坐标不转）。
       ⚠ **不要**改用 lv_display_set_rotation()：那是 LVGL 的软件旋转，
       在绘制管线里逐 draw task 做矩阵变换，S3 时代实测帧率暴跌；
       这里让 LVGL 保持 460x460 竖屏渲染，只在刷屏前转一次缓冲。
       必须在 LVGL 任务创建之前初始化（失败会自动退回未旋转路径，不阻断开机）。
       初始角度来自 NVS（开发者选项里可运行时切换，见
       lvgl_port_set_display_rotation）。 */
    if (!lvgl_ppa_rotate_init(panel_handle, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                              s_draw_buf_bytes, config_store_get_display_rotation())) {
        ESP_LOGW(TAG, "PPA 旋转不可用，画面保持未旋转");
    }

    // 设置旋转角度
    // lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90); // 软件旋转（慢，勿用）
    // ↑ 需求（画面顺时针转 90°）由上面的 lvgl_ppa_rotate_init() 在 flush 层用
    //   PPA 硬件完成；LVGL 自身的软件旋转会把旋转塞进每个 draw task 的矩阵变换里，
    //   实测帧率暴跌（S3 时代根因），不要再打开。


    /* tick 已在 lv_init() 之后用 lv_tick_set_cb() 接到硬件时间上，
       这里**不再**创建 esp_timer 定时器喂 lv_tick_inc —— 那正是整屏
       定格的根因，详见 lvgl_port_tick_cb 上方的长注释。 */

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    // 注册触摸输入设备：无条件注册（触摸可能异步初始化，传入句柄为 NULL），
    // read_cb 内部判空，句柄就绪前不给数据；就绪后经 lvgl_port_set_touch_handle 生效
    ESP_LOGI(TAG, "Register touch input device to LVGL");
    /* v9：输入设备同样改为动态创建 + setter；轮询周期取 LV_DEF_REFR_PERIOD
       （v8 时代由 LV_INDEV_DEF_READ_PERIOD 单独控制，v9 已合并，见 sdkconfig.defaults） */
    s_indev = lv_indev_create();
    assert(s_indev);
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_touchpad_read_cb);
    lv_indev_set_user_data(s_indev, touch_handle);

    // 将 LVGL 任务固定到核心 0，避免在多核调度下与其它任务竞争导致 UI 抖动
    xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE,
                            NULL, EXAMPLE_LVGL_TASK_PRIORITY, &s_lvgl_task, 0);

    /* 触摸区域测试（运行时开关，NVS）：config_store 的 touch_test（默认关，
       在 设置 → 关于 → 开发者选项 中修改，开机读取、重启生效）为 true 时
       创建白色圆点跟随手指。config_store_init 先于 lvgl_port_init 调用，
       此处读取的是本次开机固化后的值。 */
    if (config_store_get_touch_test()) {
        touch_test_create();
        ESP_LOGI(TAG, "Touch test dot enabled (developer option on)");
    } else {
        ESP_LOGI(TAG, "Touch test dot disabled (developer option off)");
    }
}

/* 运行时切换画面旋转角度（0/90/180/270，顺时针）。
   画面与触摸一起切换（触摸 read_cb 读的是同一个"实际生效角度"）；切换后
   必须整屏重绘，否则旧方向的残留块会和新方向的块混在一起（面板 RAM 没有
   "整体翻转"能力，只能重画一遍）。
   必须在 LVGL 任务上下文/持锁时调用（与 flush 回调同线程，避免读写竞争；
   设置菜单的点击回调就满足这个条件）。 */
bool lvgl_port_set_display_rotation(uint16_t cw_deg)
{
    if (!lvgl_ppa_rotate_set_angle(cw_deg)) {
        return false;
    }
    /* 整屏 + 顶层/系统层（下拉控制中心、Toast、LVGL 性能监视器都挂在那里）
       全部失效，让下一帧把整屏按新方向重画。 */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
    ESP_LOGI(TAG, "display rotation -> %u deg (touch not rotated)", (unsigned)cw_deg);
    return true;
}

/* 画面旋转是否可用（PPA 初始化成功；与当前是否为 0° 无关） */
bool lvgl_port_display_rotation_available(void)
{
    return lvgl_ppa_rotate_available();
}

void lvgl_port_suspend(void)
{
    /* 1. 冻结 LVGL 时间：记下冻结起点，之后 lv_tick_get() 停在这一刻，
          恢复后动画/定时器从暂停点继续（不会把挂起时长一次性补跑）。
          实现见 lvgl_port_tick_cb —— **不再**去 stop 一个 esp_timer。
          重复挂起（如息屏后睡眠定时器再走关机）时保留最早的起点。 */
    if (s_tick_pause_start_us < 0) {
        s_tick_pause_start_us = esp_timer_get_time();
    }
    /* 2. 挂起 LVGL 任务：停止刷屏与触摸轮询。
           息屏/深度休眠前调用；deepsleep 场景无需恢复（唤醒后重启），
           息屏场景用 lvgl_port_resume 恢复。 */
    if (s_lvgl_task != NULL) {
        vTaskSuspend(s_lvgl_task);
        ESP_LOGI(TAG, "LVGL task suspended");
    }
}

void lvgl_port_resume(void)
{
    /* 1. 解冻 LVGL 时间：把整段冻结时长记进累计偏移，tick 从暂停点继续。
          ⚠ 必须先于 vTaskResume —— 恢复后 LVGL 任务第一件事就是读 tick。 */
    if (s_tick_pause_start_us >= 0) {
        s_tick_paused_total_us += esp_timer_get_time() - s_tick_pause_start_us;
        s_tick_pause_start_us = -1;
    }
    /* 2. 恢复 LVGL 任务：重新开始刷屏与触摸轮询。
           任务恢复后若尝试获取 LVGL 互斥锁会等待调用方释放（见调用方流程） */
    if (s_lvgl_task != NULL) {
        vTaskResume(s_lvgl_task);
        ESP_LOGI(TAG, "LVGL task resumed");
    }
}
