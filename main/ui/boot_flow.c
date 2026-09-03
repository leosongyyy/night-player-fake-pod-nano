#include "ui/boot_flow.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/battery.h"
#include "core/player/player_controller.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist_builder.h"
#include "core/storage/playlist_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/app.h"

static const char *TAG = "boot_flow";

/* 前向声明（各界面函数互相调用，定义顺序有交叉） */
static void boot_show_no_music(void);
static void boot_enter_normal(void);
static void boot_show_scanning(void);
static void boot_progress_timer_cb(lv_timer_t *timer);
static void boot_scan_task(void *arg);
static void start_scan_btn_cb(lv_event_t *e);
static void clear_scan_data_btn_cb(lv_event_t *e);
static void reset_position_btn_cb(lv_event_t *e);

/* 开机最低电池电压门槛：直接引用 battery.h 的公共临界低电压常量
   （BATTERY_CRITICAL_LOW_MV，3.2V）——与运行中低电自动伪关机（power_service）
   共用同一阈值，改动只在 battery.h 一处。 */

/* 扫描进度界面刷新周期（ms） */
#define BOOT_PROGRESS_TIMER_MS 300u

/* 扫描任务栈：内部 RAM 32KB（解析 .pl 需大局部数组；内部 RAM 栈避免 mmap 断言） */
#define BOOT_SCAN_STACK_WORDS (32768u / 4u)

/* ------------------------------------------------------------------ */
/* 状态与共享扫描进度                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    BOOT_ST_NONE = 0,
    BOOT_ST_LOW_BAT,      /* 电池电压过低，拒绝启动（停在此屏） */
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
static volatile bool     s_scan_writing    = false;  /* 是否进入"写入分区"阶段 */

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

/* 1. 电池电压过低：拒绝启动，只展示警告 + 当前电压 + 提示（不自动重启） */
static void boot_show_low_battery(void)
{
    s_state = BOOT_ST_LOW_BAT;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, LV_SYMBOL_WARNING " 电池电压过低");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *warn = boot_make_hint(root, "请充电");
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, -40);

    /* 展示当前电池电压 */
    char buf[32];
    float vbat = battery_read_voltage_mv();
    if (vbat < 0) {
        snprintf(buf, sizeof(buf), "当前电压：--.- V");
    } else {
        snprintf(buf, sizeof(buf), "当前电压：%.2f V", vbat / 1000.0f);
    }
    s_status_label = boot_make_title(root, buf);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint = boot_make_hint(root, "充电完成后重启设备");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 120);

    ESP_LOGI(TAG, "low battery: refusing to boot, voltage=%.0f mV",
             (double)(vbat < 0 ? 0 : vbat));
}

/* 2. 未插入 SD 卡：提示 + failsafe 按钮组（重置歌曲位置 / 清除扫描数据），
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

/* 4. 扫描进行中：每 300ms 更新已扫描音乐数量；写入阶段提示"正在写入扫描数据" */
static void boot_show_scanning(void)
{
    s_state = BOOT_ST_SCANNING;
    s_scan_song_count = 0;
    s_scan_done = false;
    s_scan_ok = false;
    s_scan_writing = false;

    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "正在扫描音乐...");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -120);

    s_count_label = boot_make_title(root, "0");
    lv_obj_align(s_count_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *unit = boot_make_hint(root, "首");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 60);

    /* 底部状态文字：扫描期间为"正在扫描..."，进入写入阶段后切换为"正在写入扫描数据..." */
    s_status_label = boot_make_hint(root, "正在扫描...");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);

    /* 每 300ms 更新进度，并检查扫描是否完成以切换界面 */
    lv_timer_create(boot_progress_timer_cb, BOOT_PROGRESS_TIMER_MS, NULL);

    /* 启动后台扫描任务（32KB 内部 RAM 栈） */
    BaseType_t ret = xTaskCreatePinnedToCore(boot_scan_task, "boot_scan",
                                             BOOT_SCAN_STACK_WORDS, NULL, 5,
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

    app_ui_create();

    ESP_LOGI(TAG, "enter normal UI");
}

/* ------------------------------------------------------------------ */
/* 扫描后台任务与进度回调                                               */
/* ------------------------------------------------------------------ */

/* 扫描进度回调（扫描任务上下文执行，只更新共享变量，不碰 LVGL） */
static void boot_scan_progress_cb(playlist_build_phase_t phase, uint32_t song_count,
                                  void *ctx)
{
    (void)ctx;
    if (phase == PLAYLIST_BUILD_PHASE_WRITE) {
        s_scan_writing = true;
    }
    s_scan_song_count = song_count;
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
       直接进入常规界面（无音乐仍应重新提示扫描） */
    if (ret == ESP_OK && stats.song_count == 0) {
        playlist_store_invalidate();
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

    /* 写入阶段提示（写入时数量不再变化，切到状态提示） */
    if (s_scan_writing && s_status_label != NULL) {
        lv_label_set_text(s_status_label, "正在写入扫描数据...");
        s_scan_writing = false;   /* 只更新一次即可 */
    }

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

void boot_flow_run(void)
{
    /* 1. 电池电压检查：低于阈值拒绝启动 */
    float vbat = battery_read_voltage_mv();
    if (vbat >= 0 && vbat < (float)BATTERY_CRITICAL_LOW_MV) {
        ESP_LOGW(TAG, "battery too low (%.0f mV < %u mV), refuse to boot",
                 (double)vbat, BATTERY_CRITICAL_LOW_MV);
        boot_show_low_battery();
        return;
    }

    /* 2. SD 卡检查：未挂载 → 无卡界面（failsafe 清除扫描数据） */
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        boot_show_no_sd();
        return;
    }

    /* 3. 播放列表数据检查 */
    if (!playlist_store_is_valid()) {
        ESP_LOGW(TAG, "playlist data invalid, prompt to scan");
        boot_show_prompt_scan();
        return;
    }

    /* 4. 有卡 + 数据有效：直接进入常规界面 */
    boot_enter_normal();
}
