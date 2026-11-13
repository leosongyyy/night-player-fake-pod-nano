#include "ui/boot_flow.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/image/default_cover.h"
#include "core/player/player_controller.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist_builder.h"
#include "core/storage/playlist_store.h"
#include "core/ui/lvgl_port.h"   /* lvgl_port_lock/unlock：写入分区时冻结 LVGL */
#include "core/utils/mem_trace.h"
#include "core/utils/sleep_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/app.h"
#include "ui/toast.h"

static const char *TAG = "boot_flow";

/* 前向声明（各界面函数互相调用，定义顺序有交叉） */
static void boot_show_no_music(void);
static void boot_enter_normal(void);
static void boot_show_scanning(void);
static void boot_progress_timer_cb(lv_timer_t *timer);
static void boot_scan_task(void *arg);
static void boot_sleep_toast_timer_cb(lv_timer_t *timer);
static void start_scan_btn_cb(lv_event_t *e);
static void clear_scan_data_btn_cb(lv_event_t *e);
static void reset_position_btn_cb(lv_event_t *e);

/* S31 平台：电池信息已由 AXP2101 提供（电压/电量/充电状态，见 battery.c），
   但 S3 时代的"低电拒启"策略尚未接回；开机流程目前只剩 SD 卡/播放列表数据
   检查。接回时只需在 boot_flow_run 开头按 battery_read_info 判电压/电量。 */

/* 扫描进度界面刷新周期（ms） */
#define BOOT_PROGRESS_TIMER_MS 300u

/* 扫描任务栈：32KB（解析 .pl 需大局部数组）。
   ⚠ 栈**必须在内部 RAM**：扫描过程会调用 pldata_write/pldata_erase_range，
   其中 pldata_unmap_if_mapped() → esp_partition_munmap → esp_mmu_unmap 会
   走"关缓存"流程（esp_cache_freeze_caches_disable_interrupts 断言
   s_task_stack_is_sane_when_cache_frozen()：关缓存期间 PSRAM 不可访问，
   栈在 PSRAM 会直接 assert 崩溃）。内部 32KB 栈在开机早期仍可分配
   （此时 UI 尚未创建，内部 RAM 还有约 130KB 空闲）。 */
#define BOOT_SCAN_STACK_BYTES (32768u)

/* 开机睡眠定时提醒（FUTURE_PLAN 第 27 点）：
   - 延迟：主界面创建后先让首帧画出来、屏幕亮度恢复，再弹提醒，避免提示与
     开机首帧挤在同一帧；延迟期间用户已手动关闭定时则不打扰；
   - 显示时长：比普通 Toast（2 秒）长，确保用户注意到"定时已开启"并能主动
     去播放设置页关闭（这正是本提醒的目的：不需要联网对时做夜间自动开启）。 */
#define BOOT_SLEEP_TOAST_DELAY_MS 800u
#define BOOT_SLEEP_TOAST_MS       4000u

/* ------------------------------------------------------------------ */
/* 状态与共享扫描进度                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    BOOT_ST_NONE = 0,
    BOOT_ST_SPLASH,       /* 开机封面（默认封面，屏幕最早点亮的那一屏） */
    BOOT_ST_NO_SD,        /* 未插入 SD 卡（停在此屏） */
    BOOT_ST_PROMPT_SCAN,  /* 提示扫描，等待用户点击开始扫描 */
    BOOT_ST_SCANNING,     /* 正在扫描（进度每 300ms 更新） */
    BOOT_ST_NO_MUSIC,     /* 扫描后无音乐（停在此屏） */
    BOOT_ST_NORMAL,       /* 进入常规界面 */
} boot_state_t;

static boot_state_t s_state = BOOT_ST_NONE;

/* 扫描任务与 LVGL 定时器之间共享的进度（volatile，扫描任务只写、定时器只读） */
static volatile uint32_t s_scan_song_count = 0;
static volatile bool     s_scan_done       = false;
static volatile bool     s_scan_ok         = false;
/* 写入分区阶段是否已由扫描任务持住 LVGL 锁（见 boot_scan_progress_cb）：
   持锁期间 LVGL 完全冻结，写完 flash 后由 boot_scan_task 负责归还。 */
static volatile bool     s_scan_lvgl_locked = false;

/* LVGL 对象句柄 */
static lv_obj_t   *s_root           = NULL;   /* 开机 UI 根容器（全屏黑底） */
static lv_obj_t   *s_count_label    = NULL;   /* 扫描进度数字 */
static lv_obj_t   *s_status_label   = NULL;   /* 可复用的状态/提示文字 */

/* ------------------------------------------------------------------ */
/* 基础 UI 辅助                                                        */
/* ------------------------------------------------------------------ */

/* 重建开机 UI 根容器（全屏、黑底、无滚动）。会删除旧的 s_root 及其子对象，
   因此任何指向子对象的句柄都要重新创建（这里统一复位） */
static lv_obj_t *boot_root_create(void)
{
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_count_label  = NULL;
    s_status_label = NULL;

    lv_obj_t *scr = lv_scr_act();
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    return s_root;
}

/* 在 parent 中创建一个居中的大标题文字 */
static lv_obj_t *boot_make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    return lbl;
}

/* 在 parent 中创建一个副标题/说明文字（居中、灰色） */
static lv_obj_t *boot_make_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_GREY), 0);
    return lbl;
}

/* 在 parent 中创建一个带事件回调的按钮 */
static lv_obj_t *boot_make_button(lv_obj_t *parent, const char *text,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 320, 64);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_BLUE, 2),
                              LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/* ------------------------------------------------------------------ */
/* 各开机界面                                                           */
/* ------------------------------------------------------------------ */

/* 0. 开机封面（splash）：全屏黑底 + 默认封面。
   在 app_main **早期**调用（display_init + lvgl_port_init 之后、touch/audio/
   pldata/SD 之前，持 LVGL 锁）：屏幕亮度此时仍为 0，调用方在本函数返回后
   lv_refr_now() 提交首帧、再点亮，防白屏顺序与原来一致。
   目的：让用户开机即有画面，把其余硬件初始化与主界面创建（app_ui_create
   实测约 700ms）的耗时全部藏在这张封面背后（实测首次点亮从约 1.9s 提前到
   约 0.7s）。
   ⚠ 本容器会被随后 boot_flow_run() 的任一分支经 boot_root_create() 删除并
   替换成对应界面（低电/无卡/提示扫描/扫描中/无音乐/常规主界面），因此不需要
   额外的清理入口；也必须保持"整屏不透明"，否则未初始化的面板 RAM 会透出来。 */
void boot_splash_show(void)
{
    s_state = BOOT_ST_SPLASH;
    lv_obj_t *root = boot_root_create();

    /* 默认封面（DEFAULT_COVER_IMG，随 CONFIG_UI_COVER_FLASH 切换实现）：
       460×460 与屏幕等大，居中即铺满整屏 */
    lv_obj_t *cover = lv_img_create(root);
    lv_img_set_src(cover, DEFAULT_COVER_IMG);
    lv_obj_center(cover);

    ESP_LOGI(TAG, "boot splash: default cover shown");
}

/* 1. 未插入 SD 卡：提示 + failsafe 按钮组（重置歌曲位置 / 清除扫描数据），
      停在此屏。内容整体相对屏幕中心上移（基准偏移 -30），视觉居中偏上，
      给底部留白（460px 屏去掉标题与两按钮后仍不显拥挤） */
static void boot_show_no_sd(void)
{
    s_state = BOOT_ST_NO_SD;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, LV_SYMBOL_SD_CARD " 未插入 SD 卡");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -130);

    s_status_label = boot_make_hint(root, "请插入 SD 卡后重启设备");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -30);

    /* failsafe 按钮组（上下排布，避开中央提示文字）：
       - 重置歌曲位置：清 NVS 里"上次播放歌曲/随机位置"记录 → 下次插卡开机
         从第一首开始（不恢复可能已损坏/随卡丢失的现场，见
         config_store_reset_position）
       - 清除扫描数据：擦除 pldata 头部，使内部扫描数据分区无效 */
    lv_obj_t *btn_reset = boot_make_button(root, "重置歌曲位置", reset_position_btn_cb);
    lv_obj_align(btn_reset, LV_ALIGN_CENTER, 0, 55);

    lv_obj_t *btn_clear = boot_make_button(root, "清除扫描数据", clear_scan_data_btn_cb);
    lv_obj_align(btn_clear, LV_ALIGN_CENTER, 0, 145);

    /* 进入 failsafe 界面即把亮度拉满（255）并持久化到 NVS：
       无卡场景可能带着上次的低亮度（甚至全黑）开机，拉满便于看清界面/
       操作"清除扫描数据"。boot_flow_run 在 app_main 的 LVGL 锁内调用，
       此处调 display_set_brightness 安全；app_main 随后点亮时读到的
       config_store 亮度也已是 255（set 会写 NVS 并通知回调，boot 阶段
       UI 未创建，无回调副作用）。 */
    config_store_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);
    display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);
}

/* 3. 有卡但数据无效：提示扫描 + "开始扫描"按钮（无拒绝选项，只能关机） */
static void boot_show_prompt_scan(void)
{
    s_state = BOOT_ST_PROMPT_SCAN;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "需要扫描音乐");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -100);

    lv_obj_t *hint = boot_make_hint(root, "内部播放列表数据无效，请扫描 SD 卡");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *btn = boot_make_button(root, "开始扫描", start_scan_btn_cb);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 130);
}

/* 4. 扫描进行中：每 300ms 更新已扫描音乐数量；进入写入阶段时由扫描任务
      抢住 LVGL 锁并提示"正在写入扫描数据"（见 boot_scan_progress_cb） */
static void boot_show_scanning(void)
{
    s_state = BOOT_ST_SCANNING;
    s_scan_song_count = 0;
    s_scan_done = false;
    s_scan_ok = false;
    s_scan_lvgl_locked = false;

    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "正在扫描音乐...");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -120);

    s_count_label = boot_make_title(root, "0");
    lv_obj_align(s_count_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *unit = boot_make_hint(root, "首");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 60);

    /* 底部状态文字：扫描期间为"正在扫描..."；进入写入阶段后由扫描任务在持有
       LVGL 锁的情况下改写为"正在写入扫描数据..."并立刻提交该帧（此后界面冻结） */
    s_status_label = boot_make_hint(root, "正在扫描...");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);

    /* 每 300ms 更新进度，并检查扫描是否完成以切换界面 */
    lv_timer_create(boot_progress_timer_cb, BOOT_PROGRESS_TIMER_MS, NULL);

    /* 启动后台扫描任务（32KB 栈，⚠ 必须内部 RAM：扫描会触发 pldata 的
       munmap/erase 关缓存流程，PSRAM 栈会 assert 崩溃，见上方常量注释） */
    BaseType_t ret = xTaskCreatePinnedToCore(boot_scan_task, "boot_scan",
                                             BOOT_SCAN_STACK_BYTES / 4u, NULL, 5,
                                             NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        boot_show_no_music();   /* 任务创建失败，当作扫描失败处理 */
    }
}

/* 5. 扫描后无音乐：提示信息，停在此屏直到重启 */
static void boot_show_no_music(void)
{
    s_state = BOOT_ST_NO_MUSIC;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "未发现音乐");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *hint = boot_make_hint(root, "SD 卡中没有可播放的 mp3 文件");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint2 = boot_make_hint(root, "请放入音乐后重启设备");
    lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 100);
}

/* 6. 进入常规界面：删除开机 UI，创建应用主界面 */
static void boot_enter_normal(void)
{
    s_state = BOOT_ST_NORMAL;

    /* 系统字体已在 app_main 早期由 system_font_init() 挂载好（NPF1 全字库或
       内嵌精简字库兜底），这里不需要再加载。UI 创建时按 UI_SYSTEM_FONT 的
       行高（NPF 24px 行高 32）算列表行高，所以字体必须先就绪。 */

    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
        s_count_label = NULL;
        s_status_label = NULL;
    }
    /* 先加载播放列表并启动播放控制器，再创建应用 UI：这样主界面的虚拟列表
       在创建时即可直接读取真实目录数据（playlist_dir_count 等）。播放控制
       命令走队列异步执行（player_controller_init 仅投递"播放第一首"命令），
       不会阻塞 UI 创建。 */
    esp_err_t r = player_controller_init();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "player controller init failed: %s", esp_err_to_name(r));
    }
    mem_trace_log("ui:after player_ctrl");

    mem_trace_log("ui:before app_ui_create");
    app_ui_create();
    mem_trace_log("ui:after app_ui_create");

    /* 开机默认开启睡眠定时（设置 → 睡眠定时器 → 开机默认开启）时，主界面
       就绪后弹 Toast 提醒"定时已开启"（见 FUTURE_PLAN 第 27 点）：用户由此
       能主动发现定时开着、在不需要时手动关闭，因此不必再依赖联网对时实现
       "夜间自动开启"。判定与 main.c 真正启动定时共用同一函数。 */
    uint32_t boot_sleep_min = sleep_timer_boot_autostart_min();
    if (boot_sleep_min > 0) {
        lv_timer_create(boot_sleep_toast_timer_cb, BOOT_SLEEP_TOAST_DELAY_MS,
                        (void *)(uintptr_t)boot_sleep_min);
        ESP_LOGI(TAG, "boot: sleep timer reminder scheduled (%u min)",
                 (unsigned)boot_sleep_min);
    }

    ESP_LOGI(TAG, "enter normal UI");
    mem_trace_log("ui:enter normal done");
}

/* ------------------------------------------------------------------ */
/* 扫描后台任务与进度回调                                               */
/* ------------------------------------------------------------------ */

/* 扫描进度回调（扫描任务上下文；**写入阶段会短暂操作 LVGL**，见下） */
static void boot_scan_progress_cb(playlist_build_phase_t phase, uint32_t song_count,
                                  void *ctx)
{
    (void)ctx;
    s_scan_song_count = song_count;

    if (phase != PLAYLIST_BUILD_PHASE_WRITE) {
        return;
    }

    /* ⚠⚠ 写入阶段（playlist_store_save_playlist → 擦写 pldata 分区）会**关闭外部
       flash 缓存**，此时所有"映射在 flash 上的数据"都不可访问——本项目的系统
       字体正是 NPF1 全字库：`font_npf.c` 用 esp_partition_mmap 把 font 分区直接
       映射进来，LVGL 渲染文字时（draw_main 里取字形位图）就是读这块 flash。若
       LVGL 在写入期间还在渲染，取字形的总线访问会卡在"缓存已关、flash 忙"状态里
       出不来：**LVGL 任务永久卡死**（实测：task_wdt 在 5s 后报 "CPU 0: LVGL" 并
       打出 lv_label 绘制 → lv_refr 的调用栈；WDT 关闭时表现为扫描完成后界面再也
       不动、日志刷 task_wdt，只能断电重启。详见 ideas_and_exps/NPF_FONT.md §7.1）。
       修法与 TTF 时代（commit 255f0b9）一致：**先把 LVGL 锁拿过来**让渲染停下来，
       并主动提交"正在写入…"这一帧；锁交给 boot_scan_task 在写完 flash（含无音乐
       时的 playlist_store_invalidate）之后归还。
       ⚠ 本回调跑在扫描任务上下文（Core1，栈在内部 RAM——关缓存期间栈必须可访问）。
       ⚠ 持锁期间本任务只做 flash 擦写，不再碰 PSRAM/LVGL。 */
    if (lvgl_port_lock(-1)) {
        s_scan_lvgl_locked = true;

        if (s_count_label != NULL) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", (unsigned)s_scan_song_count);
            lv_label_set_text(s_count_label, buf);
        }
        if (s_status_label != NULL) {
            lv_label_set_text(s_status_label, "正在写入扫描数据...");
        }
        lv_refr_now(NULL);   /* 冻结前把最后一帧真正画出去 */
        ESP_LOGI(TAG, "write phase: LVGL locked, UI frozen until write finishes");
    } else {
        ESP_LOGW(TAG, "write phase: LVGL lock failed, UI may glitch during write");
    }
}

/* 扫描后台任务：调用构建器扫描并保存，结束后置完成标志 */
static void boot_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "scan task begin");

    playlist_build_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    esp_err_t ret = playlist_build_from_sd(&stats, boot_scan_progress_cb, NULL);
    ESP_LOGI(TAG, "scan done: dirs=%u songs=%u ret=%s",
             (unsigned)stats.dir_count, (unsigned)stats.song_count,
             esp_err_to_name(ret));

    /* 若扫描成功但没有任何音乐：清除分区有效性，避免下次重启误判为"有数据"而
       直接进入常规界面（无音乐仍应重新提示扫描）。
       ⚠ 这里同样是擦 flash（关缓存），必须在**归还 LVGL 锁之前**做完。 */
    if (ret == ESP_OK && stats.song_count == 0) {
        playlist_store_invalidate();
    }

    /* 写入阶段结束：归还 LVGL 锁，界面恢复渲染（进度定时器随即切到主界面/无音乐页）。
       顺序很关键：必须在置 s_scan_done 之前解锁——定时器要靠这把锁才能跑起来。 */
    if (s_scan_lvgl_locked) {
        s_scan_lvgl_locked = false;
        lvgl_port_unlock();
        ESP_LOGI(TAG, "write phase: LVGL unlocked, UI resumes");
    }

    s_scan_ok = (ret == ESP_OK);
    s_scan_done = true;
    vTaskDelete(NULL);
}

/* 扫描进度定时器（LVGL 任务上下文，已持锁）：每 300ms 更新数量；
   进入写入阶段后提示"正在写入扫描数据"；扫描完成则切换界面 */
static void boot_progress_timer_cb(lv_timer_t *timer)
{
    if (s_state != BOOT_ST_SCANNING) {
        return;
    }

    if (s_count_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", (unsigned)s_scan_song_count);
        lv_label_set_text(s_count_label, buf);
    }

    /* 注：进入写入阶段后不再由这里更新"正在写入…"文案——那一刻扫描任务已经
       抢走 LVGL 锁并自己把这一帧画完了（见 boot_scan_progress_cb）。 */

    if (!s_scan_done) {
        return;   /* 扫描尚未结束 */
    }

    lv_timer_del(timer);   /* 停止本定时器 */

    if (s_scan_song_count == 0) {
        boot_show_no_music();
    } else {
        boot_enter_normal();
    }
}

/* 开机睡眠定时提醒定时器（一次性，LVGL 任务上下文已持锁）：弹 Toast 后自删。
   user_data 携带开机自动开启的分钟数（boot_enter_normal 创建时传入）。 */
static void boot_sleep_toast_timer_cb(lv_timer_t *timer)
{
    /* v9 的 lv_timer_t 是不透明类型，user_data 只能通过 getter 读
       （v8 时代是公开字段）；先取出参数再自删定时器 */
    uint32_t minutes = (uint32_t)(uintptr_t)lv_timer_get_user_data(timer);
    lv_timer_del(timer);   /* 一次性定时器 */

    /* 延迟期间用户可能已手动关闭定时（播放设置页长按睡眠格子）：
       已关闭则不再打扰 */
    if (!sleep_timer_is_active()) {
        ESP_LOGI(TAG, "boot: sleep timer already off, skip reminder");
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "睡眠定时已开启 %u 分钟", (unsigned)minutes);
    ui_toast_show_ms(buf, BOOT_SLEEP_TOAST_MS);
    ESP_LOGI(TAG, "boot: sleep timer reminder toast (%u min)", (unsigned)minutes);
}

/* ------------------------------------------------------------------ */
/* 按钮事件回调（LVGL 任务上下文，已持锁）                              */
/* ------------------------------------------------------------------ */

/* "开始扫描"：进入扫描界面（界面与后台任务在 boot_show_scanning 中创建） */
static void start_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "start scan button pressed");
    boot_show_scanning();
}

/* "清除扫描数据"（failsafe）：擦除 pldata 头部使分区无效，界面停留并提示 */
static void clear_scan_data_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "clear scan data button pressed");
    esp_err_t ret = playlist_store_invalidate();
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label,
                          ret == ESP_OK ? "已清除扫描数据" : "清除失败，请重试");
    }
}

/* "重置歌曲位置"（failsafe）：清掉 NVS 里上次播放歌曲与随机播放位置记录，
   供"换卡/歌曲缺失/上次现场引用了损坏文件"等场景从干净状态重新开始；
   下次插卡开机会从第一首播放（随机模式重新生成起点），界面停留并提示 */
static void reset_position_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "reset position button pressed");
    config_store_reset_position();
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, "已重置歌曲位置");
    }
}

/* ------------------------------------------------------------------ */
/* 开机流程入口                                                        */
/* ------------------------------------------------------------------ */

/* ⚠ 系统字体在 app_main 早期已挂载（NPF1 全字库，mmap 只读）：本流程的所有
   分支（无卡 / 待扫描 / 扫描 / 常规界面）都用同一份字体，不再有 TTF 时代的
   "延迟加载"。
   · 内嵌精简字库仍作为 NPF 挂载失败时的兜底（system_font.c），覆盖全部 UI
     固定文案（见 ideas_and_exps/FontSymbols.md）；
   · "数据无效"这一支会扫描 SD 卡并整体重写 pldata 分区，写入阶段会关闭外部
     flash 缓存——此时 mmap 在 font 分区上的字形位图不可访问，所以**写入期间必须
     让 LVGL 停止渲染**（见 boot_scan_progress_cb 的持锁逻辑与 NPF_FONT.md §7.1）。 */
void boot_flow_run(void)
{
    /* 1. SD 卡检查：未挂载 → 无卡界面（failsafe 清除扫描数据） */
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        boot_show_no_sd();
        return;
    }

    /* 2. 播放列表数据检查 */
    if (!playlist_store_is_valid()) {
        ESP_LOGW(TAG, "playlist data invalid, prompt to scan");
        boot_show_prompt_scan();
        return;
    }

    /* 3. 有卡 + 数据有效：直接进入常规界面 */
    boot_enter_normal();
}
