#include "ui/boot_flow.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/axp2101.h"
#include "core/hardware/battery.h"
#include "core/image/default_cover.h"
#include "core/player/player_controller.h"
#include "core/hardware/display.h"
#include "core/hardware/pldata.h"    /* pldata_has_data：FORCE_RESCAN 调试分支判断有无缓存 */
#include "core/hardware/sd_card.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist_builder.h"
#include "core/storage/playlist_store.h"
#include "core/ui/lvgl_port.h"   /* lvgl_port_lock/suspend：充电界面刷新与息屏 */
#include "core/utils/deep_worker.h"   /* 充电界面息屏/亮屏的深链 */
#include "core/utils/mem_trace.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/sleep_timer.h"
#include "core/utils/task_psram.h"
#include "driver/gpio.h"   /* 充电界面的下键（GPIO61）"任意键亮屏"用 */
#include "esp_log.h"
#include "esp_system.h"   /* esp_restart()：充电界面短按上键进入正常系统 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/app.h"
#include "ui/toast.h"

static const char *TAG = "boot_flow";

/* idfc quickconfig（`idfc build -qc <名字>` 生成，见 idfc_buildconfig.json 的
   QuickConfigs）：临时调试开关走这里，不走 Kconfig、不触发 reconfigure。
   ⚠ 当前已知坑：idfc 每次构建都会重写这个头文件，而 ccache 的"缺失头文件"
   记录不会随头文件出现而失效——首次启用某个 quickconfig 后，如果对应 .c 的
   内容没变，可能拿到**旧的缓存对象**（开关看着开了、实际没编进去）。遇到这种
   情况给那个 .c 加一行注释（改内容）再 build 即可。 */
#if defined(__has_include)
#  if __has_include("idfc_quickconfig.h")
#    include "idfc_quickconfig.h"
#  endif
#endif
#ifndef QCONF_FORCE_RESCAN
#define QCONF_FORCE_RESCAN 0
#endif

/* 前向声明（各界面函数互相调用，定义顺序有交叉） */
static void boot_show_no_music(void);
static void boot_show_scan_failed(void);
static void boot_enter_normal(void);
static void boot_show_scanning(void);
static void boot_progress_timer_cb(lv_timer_t *timer);
static void boot_scan_task(void *arg);
static void boot_sleep_toast_timer_cb(lv_timer_t *timer);
static void reset_position_btn_cb(lv_event_t *e);

/* S31 平台：电池信息已由 AXP2101 提供（电压/电量/充电状态，见 battery.c），
   但 S3 时代的"低电拒启"策略尚未接回；开机流程目前只剩 SD 卡/播放列表数据
   检查。接回时只需在 boot_flow_run 开头按 battery_read_info 判电压/电量。 */

/* 扫描进度界面刷新周期（ms） */
#define BOOT_PROGRESS_TIMER_MS 300u

/* 扫描任务栈：32KB（解析 .pl 需大局部数组）。
   ⚠ 2026-11-16 起栈放 **PSRAM**（task_create_psram_stack）：播放列表数据换成
   SD 卡上的缓存文件后，扫描任务不再触发任何"关外部缓存"流程——只有 flash 擦写
   与分区 map/unmap 才关缓存，写 SD 只是文件 I/O。所以关缓存断言
   s_task_stack_is_sane_when_cache_frozen() 不可能被命中。
   顺带把开机最紧张时刻的内部 RAM（原来向 32KB 要、常退回 16KB）腾给 LVGL/驱动。 */
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
    BOOT_ST_LOW_BAT,      /* 电量过低且未充电：拒绝启动（停在此屏） */
    BOOT_ST_NO_SD,        /* 未插入 SD 卡（停在此屏） */
    BOOT_ST_SCANNING,     /* 正在扫描（进度每 300ms 更新） */
    BOOT_ST_NO_MUSIC,     /* 扫描后无音乐（停在此屏） */
    BOOT_ST_NORMAL,       /* 进入常规界面 */
    BOOT_ST_CHARGING,     /* 充电专用界面（插充电器开机；拔电即 PMU 关机） */
} boot_state_t;

static boot_state_t s_state = BOOT_ST_NONE;

/* 扫描任务与 LVGL 定时器之间共享的进度（volatile，扫描任务只写、定时器只读） */
static volatile uint32_t s_scan_song_count = 0;
static volatile bool     s_scan_done       = false;
static volatile bool     s_scan_ok         = false;
/* 写入阶段标志：扫描任务只置位，真正改 UI 文案的是 300ms 的进度定时器
   （LVGL 任务上下文）。写 SD 不关外部缓存，所以**不需要**像 flash 时代那样
   抢 LVGL 锁冻结界面（见 boot_scan_progress_cb）。 */
static volatile bool     s_scan_writing    = false;

/* LVGL 对象句柄 */
static lv_obj_t   *s_root           = NULL;   /* 开机 UI 根容器（全屏黑底） */
static lv_obj_t   *s_count_label    = NULL;   /* 扫描进度数字 */
static lv_obj_t   *s_status_label   = NULL;   /* 可复用的状态/提示文字 */
static lv_timer_t *s_scan_timer     = NULL;   /* 扫描进度定时器 */

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
   替换成对应界面（低电/无卡/扫描中/无音乐/扫描失败/常规主界面），因此不需要
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

/* 1. 低电拒启（S31，2026-11 接回）：电量 < 2% 且未在充电 → 只显示一行
   「电量过低，请充电」，**停留 5 秒后由 PMU 直接断电**（不停在此界面）。
   判据用 AXP2101 的电量计百分比（battery_read_info 的融合值），并要求
   读数可用（percent != 0xFF），避免 PMU 读失败时把机器误关机。
   ⚠ 与 S3 的差异：S3 用分压 ADC 的**电压**（<3.45V）判断并停在此屏等用户
   关机；S31 没有那条分压链路，改用电量百分比，且由固件主动断电。
   ⚠ 这里不走 fake_shutdown：开机早期没有播放现场要保存，而 fake_shutdown 会
   把提示刷成「正在关机...」并挂起 LVGL，与"只显示一句提示"的诉求冲突。
   运行中的低电关机由 power_service 按 5% / 60s 判（PMU 真关机）。 */
#define BOOT_LOW_BAT_PERCENT        2
#define BOOT_LOW_BAT_SHUTDOWN_MS    5000

/* 5 秒到：一行提示已经显示完，直接让 PMU 断电。
   断电成功时整机立刻停机，不会返回；失败只记录日志（面板保留最后一句提示，
   用户可长按电源键 4 秒由 PMU 的 OFFLEVEL 硬件兜底断电，不在此处重试）。 */
static void boot_low_bat_shutdown_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 5 秒内用户插上充电器：不再关机，直接重启进"充电专用界面"
       （正常开机路径已初始化 SD/播放列表，重启最省事；esp_restart 后由
       boot_reason 判定为插电开机）。 */
    battery_info_t info;
    if (battery_read_info(&info) == ESP_OK
            && (info.vbus_present || info.charging)) {
        ESP_LOGW(TAG, "low battery: charger plugged in, restarting to charge mode");
        esp_restart();
    }

    ESP_LOGW(TAG, "low battery: 5 s elapsed, sending PMU power-off");
    esp_err_t err = axp2101_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMU power-off failed: %s", esp_err_to_name(err));
    }
}

static void boot_show_low_battery(const battery_info_t *info)
{
    s_state = BOOT_ST_LOW_BAT;
    lv_obj_t *root = boot_root_create();

    /* 只留一行居中白字：不显示电压/百分比，也不放任何按钮（屏幕 5 秒后熄灭） */
    lv_obj_t *msg = boot_make_title(root, "电量过低，请充电");
    lv_obj_center(msg);

    /* 一次性定时器：LVGL 任务上下文，5 秒后由 PMU 断电 */
    lv_timer_t *timer = lv_timer_create(boot_low_bat_shutdown_cb,
                                        BOOT_LOW_BAT_SHUTDOWN_MS, NULL);
    if (timer != NULL) {
        lv_timer_set_repeat_count(timer, 1);
    } else {
        ESP_LOGE(TAG, "failed to create low-battery shutdown timer");
    }

    ESP_LOGW(TAG, "low battery: refusing to boot (%u%%, vbat=%u mV, vbus=%d), "
                  "PMU power-off in %d ms",
             (unsigned)(info->percent == 0xFF ? 0 : info->percent),
             (unsigned)info->vbat_mv, (int)info->vbus_present,
             BOOT_LOW_BAT_SHUTDOWN_MS);
}

/* 2. 未插入 SD 卡：提示 + failsafe 按钮（重置歌曲位置），停在此屏。
      内容整体相对屏幕中心上移（基准偏移 -30），视觉居中偏上。
      ⚠ 2026-11-16 起**删掉了"清除扫描数据"按钮**：播放列表缓存已从 flash 分区
        搬到 SD 卡上的文件，没有卡的时候根本没有任何东西可清（旧按钮调
        playlist_store_invalidate() 只对那些"卡在、缓存坏"的场景有用，而那种
        场景 boot_flow 会自动重扫）。要强制重扫用设置菜单的"重新扫描"。 */
static void boot_show_no_sd(void)
{
    s_state = BOOT_ST_NO_SD;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, LV_SYMBOL_SD_CARD " 未插入 SD 卡");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -130);

    s_status_label = boot_make_hint(root, "请插入 SD 卡后重启设备");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -30);

    /* failsafe 按钮：重置歌曲位置（清 NVS 里"上次播放歌曲/随机位置"记录 →
       下次插卡开机从第一首开始，不恢复可能已损坏/随卡丢失的现场，
       见 config_store_reset_position） */
    lv_obj_t *btn_reset = boot_make_button(root, "重置歌曲位置", reset_position_btn_cb);
    lv_obj_align(btn_reset, LV_ALIGN_CENTER, 0, 55);

    /* 进入 failsafe 界面即把亮度拉满（255）并持久化到 NVS：
       无卡场景可能带着上次的低亮度（甚至全黑）开机，拉满便于看清界面/
       操作按钮。boot_flow_run 在 app_main 的 LVGL 锁内调用，
       此处调 display_set_brightness 安全；app_main 随后点亮时读到的
       config_store 亮度也已是 255（set 会写 NVS 并通知回调，boot 阶段
       UI 未创建，无回调副作用）。 */
    config_store_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);
    display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);
}

/* 3. 扫描进行中：每 300ms 更新已扫描音乐数量；进入写入阶段后定时器把底部
      文案改成"正在写入扫描数据..."（扫描任务只置 s_scan_writing 标志） */
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

    /* 底部状态文字：扫描期间为"正在扫描..."；进入写入阶段后由进度定时器
       改写为"正在写入扫描数据..."（界面不冻结，照常刷新） */
    s_status_label = boot_make_hint(root, "正在扫描...");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);

    /* 每 300ms 更新进度，并检查扫描是否完成以切换界面 */
    s_scan_timer = lv_timer_create(boot_progress_timer_cb, BOOT_PROGRESS_TIMER_MS, NULL);

    /* 启动后台扫描任务（32KB 栈，放 PSRAM：写 SD 不关外部缓存，PSRAM 栈安全，
       见 BOOT_SCAN_STACK_BYTES 注释）。task_create_psram_stack 在 PSRAM 分配
       失败时会自动退回内部 RAM。 */
    BaseType_t ret = task_create_psram_stack(boot_scan_task, "boot_scan",
                                             BOOT_SCAN_STACK_BYTES, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        /* 32KB 也拿不到时退回 16KB（历史 8KB 版本已能完成 1233 首扫描） */
        ESP_LOGW(TAG, "32KB scan stack alloc failed, retry 16KB");
        ret = task_create_psram_stack(boot_scan_task, "boot_scan",
                                      16 * 1024, NULL, 5, NULL, 1);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        if (s_scan_timer != NULL) {
            lv_timer_del(s_scan_timer);
            s_scan_timer = NULL;
        }
        boot_show_scan_failed();   /* 任务创建失败：不要带无效列表进主界面 */
    }
}

/* 5. 扫描后无音乐：提示信息，停在此屏直到重启 */
static void boot_show_no_music(void)
{
    s_state = BOOT_ST_NO_MUSIC;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "未发现音乐");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *hint = boot_make_hint(root, "SD 卡中没有可播放的 mp3 / flac / ogg / wav 文件");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint2 = boot_make_hint(root, "请放入音乐后重启设备");
    lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 100);
}

/* 扫描/写入失败：停在此屏，避免带着无效播放列表进入主界面（列表会为空） */
static void boot_show_scan_failed(void)
{
    s_state = BOOT_ST_NO_MUSIC;
    lv_obj_t *root = boot_root_create();

    lv_obj_t *title = boot_make_title(root, "扫描失败");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *hint = boot_make_hint(root, "无法读取 SD 卡或写入播放列表数据");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint2 = boot_make_hint(root, "请检查存储卡后重启设备");
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
/* 充电专用界面（插充电器自动开机）                                       */
/* ------------------------------------------------------------------ */

/* 轮询周期：200ms——拔充电器要尽快反应（PMU 已不在充电，系统靠电池跑），
   但也不必更密；1s 刷一次界面足够（电量/电压变化很慢）。 */
#define CHG_POLL_MS            50u
#define CHG_UI_REFRESH_US      (1000 * 1000)
/* 开机后宽限 1.5s 再判断"没插充电器"：避免 PMU 上电瞬间 VBUS 状态抖动导致
   刚进界面就关机。 */
#define CHG_UNPLUG_GRACE_US    (1500 * 1000)
/* 充电界面无操作自动息屏：10 秒（按任意键亮屏；亮屏后短按上键才进系统） */
#define CHG_AUTO_SCREEN_OFF_US (10 * 1000 * 1000)
/* 充电界面：上键按住满 0.6s 进入系统（与"按住电源键开机"语义一致）。
   PMU 的 IRQLEVEL 最短是 1s，所以不用 PMU 长按事件，由固件计时。 */
#define CHG_BOOT_HOLD_US       (600 * 1000)
/* 充电界面的"任意键"：上键 = PMU PWRON（无主控 GPIO）；下键 = GPIO61（低有效） */
#define CHG_DOWN_KEY_GPIO      GPIO_NUM_61
#define CHG_MON_STACK          4096u
#define CHG_MON_PRIO           4
#define CHG_MON_CORE           1   /* 与 LVGL(Core0) 分开 */

static lv_obj_t *s_chg_arc     = NULL;   /* 大环：电量 0~100（lv_arc） */
static lv_obj_t *s_chg_percent = NULL;   /* 环中心百分比 */
static lv_obj_t *s_chg_status  = NULL;   /* 环中心下方状态 */
static lv_obj_t *s_chg_care    = NULL;   /* 底部小字：电池健康保护是否启用 */
/* 关机流程互斥：拔电检测每 50ms 一次，关机失败后不要反复重试 */
static volatile bool s_chg_shutting_down = false;
/* 充电界面息屏状态（独立的轻量息屏：AMOLED SLEEP IN + 挂起 LVGL，与
   power_service 的息屏互不影响——charge-only 模式没有初始化 power_service） */
static bool     s_chg_screen_off = false;
/* 最近一次"按键活动"时刻（us）：自动息屏计时基准 */
static int64_t  s_chg_last_activity_us = 0;
/* 上一次"按住"状态与按下时刻：上键按住满 0.6s 进入系统（PMU 的 IRQLEVEL 最短
   1s，做不到 0.6s，所以由固件计时）。 */
static bool     s_chg_up_prev = false;
static int64_t  s_chg_up_press_us = 0;
/* 下键上一轮的去抖状态 */
static int      s_chg_down_stable = 1;   /* 1=抬起，0=按下（去抖后的稳定值） */
static int      s_chg_down_debounce = 0;

/* 充电界面亮度：用户配置 + 下限 128（插着充电器时至少半亮，别像没开机） */
static uint8_t charging_ui_brightness(void)
{
    uint8_t b = config_store_get_brightness();
    return (b < 128) ? 128 : b;
}

#define CHG_COLOR_ACTIVE  lv_color_hex(0xFFC000)   /* 充电中：黄（项目强调色） */
#define CHG_COLOR_FULL    lv_color_hex(0x30D158)   /* 已充满：绿 */

/* 刷新界面（**必须持 LVGL 锁**）。info==NULL 表示本次读取失败。 */
static void charging_ui_update(const battery_info_t *info)
{
    if (s_chg_arc == NULL) {
        return;
    }

    /* 底部小字：**只在保护开启时显示**「电池健康保护：开启」；关闭时整条隐藏
       （本界面无法改设置，但每次刷新都读一次，保持与 NVS 一致）。 */
    if (s_chg_care != NULL) {
        if (config_store_get_batt_care()) {
            lv_label_set_text(s_chg_care, "电池健康保护：开启");
            lv_obj_clear_flag(s_chg_care, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chg_care, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (info == NULL || info->percent == 0xFF) {
        lv_arc_set_value(s_chg_arc, 0);
        lv_obj_set_style_arc_color(s_chg_arc, lv_palette_main(LV_PALETTE_GREY),
                                   LV_PART_INDICATOR);
        if (s_chg_percent != NULL) {
            lv_label_set_text(s_chg_percent, "--%");
        }
        if (s_chg_status != NULL) {
            lv_label_set_text(s_chg_status, "电池信息不可用");
            lv_obj_set_style_text_color(s_chg_status, lv_palette_main(LV_PALETTE_GREY), 0);
        }
        return;
    }

    /* 电池健康保护生效（正在抑制充电）时：大环画成**完整一圈**表示"充电已停止"，
       不再按实际电量画弧；环中心仍显示实际电量（信息不丢）。 */
    const bool care_stopped = battery_health_protection_is_stopped();
    lv_arc_set_value(s_chg_arc, care_stopped ? 100 : info->percent);
    if (s_chg_percent != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)info->percent);
        lv_label_set_text(s_chg_percent, buf);
    }

    /* 状态文案：
       · 电池健康保护生效（充电被抑制）= "充电已停止"，不是真的充满；
       · 阶段 DONE = PMU 判定充满；
       · 保护生效时阶段是 not charging 但 VBUS 在位、电压已到上限——对用户来说
         就是"已充满"，不能显示成"未在充电"让人以为没在充。 */
    const bool full = care_stopped
                      || (info->charge_stage == AXP2101_CHG_DONE)
                      || (!info->charging && info->vbat_mv >= BATTERY_CARE_STOP_MV);
    lv_obj_set_style_arc_color(s_chg_arc, full ? CHG_COLOR_FULL : CHG_COLOR_ACTIVE,
                               LV_PART_INDICATOR);
    if (s_chg_status != NULL) {
        lv_label_set_text(s_chg_status,
                          care_stopped ? "充电已停止"
                                       : full ? "已充满"
                                              : info->charging ? "正在充电"
                                                               : "未在充电");
        lv_obj_set_style_text_color(s_chg_status,
                                    full ? CHG_COLOR_FULL : lv_color_white(), 0);
    }
}

/* 息屏"重活"（deep_worker 内部 12KB 栈执行）：AMOLED SLEEP IN → 挂起 LVGL。
   ⚠ 与 power_service 的区别：这里在**持 LVGL 锁**时挂起 LVGL 任务，再释放锁。
   若像 power_service 那样"先解锁、再 suspend"，LVGL 任务可能刚好拿到锁并卡在
   lv_timer_handler 里被挂起 → 锁永远不释放，之后所有 lvgl_port_lock 都会死等。
   持锁时挂起则保证 LVGL 任务没有持锁。 */
static void charging_screen_off_do(void *arg)
{
    (void)arg;
    if (lvgl_port_lock(-1)) {
        display_sleep_enter();
        lvgl_port_suspend();
        lvgl_port_unlock();
    }
}

/* 亮屏"重活"（deep_worker 内部栈执行）：唤醒 AMOLED（SLEEP OUT + 重发完整
   初始化）→ 全屏重刷 → 恢复亮度 → 恢复 LVGL（同样在持锁时 resume，见上）。
   ⚠ 顺序与 power_service_do_screen_on 一致：先于 LVGL 恢复完成，且刷完
   才点亮亮度（面板重初始化会清显示 RAM，必须在黑屏状态刷完整屏）。 */
static void charging_screen_on_do(void *arg)
{
    (void)arg;
    if (lvgl_port_lock(-1)) {
        display_sleep_exit();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        display_set_brightness(charging_ui_brightness());
        lvgl_port_resume();
        lvgl_port_unlock();
    }
}

/* 息屏（监控任务上下文，非 LVGL 任务）：深链委托 deep_worker，失败就地兜底
   （息屏不做全屏重刷，4KB 栈足够）。 */
static void charging_screen_off(void)
{
    if (s_chg_screen_off) {
        return;
    }
    s_chg_screen_off = true;
    if (!deep_worker_call(charging_screen_off_do, NULL)) {
        ESP_LOGW(TAG, "deep_worker unavailable, screen off in monitor task");
        charging_screen_off_do(NULL);
    }
    ESP_LOGI(TAG, "charging UI: screen off (no key for 10s)");
}

/* 亮屏：委托 deep_worker（全屏重刷需要大栈）；失败时只做最小恢复并告警。 */
static void charging_screen_on(void)
{
    if (!s_chg_screen_off) {
        s_chg_last_activity_us = esp_timer_get_time();
        return;
    }
    s_chg_screen_off = false;
    if (!deep_worker_call(charging_screen_on_do, NULL)) {
        ESP_LOGE(TAG, "deep_worker unavailable, cannot fully re-init display");
    }
    s_chg_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "charging UI: screen on (key pressed)");
}

/* 记录一次按键活动：重置自动息屏计时 */
static void charging_note_activity(void)
{
    s_chg_last_activity_us = esp_timer_get_time();
}

/* 拔掉充电器：交给 fake_shutdown 走统一关机链（deep_worker 12KB 内部栈：
   显示「充电器已断开，正在关机...」→ 挂起 LVGL → 写 PMU 0x10 断电）。
   ⚠ 充电专用模式下播放器/SD/NVS 都未初始化，流程中的保存现场/停播放/卸 SD
     都是无操作，真正生效的是"显示提示 + 挂起 LVGL + PMU 断电"。
   ⚠ 若此时是息屏状态，先亮屏再关机，否则用户看不到"正在关机"提示。
   ⚠ 本函数运行在 charging_monitor_task（PSRAM 栈）：只做投递，不在本任务里
     渲染 LVGL / 关缓存，因此安全。 */
static void charging_shutdown(void)
{
    if (s_chg_shutting_down) {
        return;
    }
    s_chg_shutting_down = true;   /* 关机失败也不重试（PMU 长按 4s 硬件兜底） */
    if (s_chg_screen_off) {
        charging_screen_on();
    }
    ESP_LOGW(TAG, "charger removed -> shutdown via PMU");
    fake_shutdown_charger_unplugged();
}

/* 充电监控任务（PSRAM 栈，Core1）：
   · 50ms 轮询上键（PMU 按住状态/短按事件）与下键（GPIO61）；
     息屏时按任意键只亮屏，亮屏后短按上键才 esp_restart 进系统；
   · 无按键 10 秒自动息屏（AMOLED SLEEP IN + 挂起 LVGL）；
   · 每 200ms 读一次 PMU 状态（电量/电压/VBUS，界面 1s 刷一次）；
   · 拔掉充电器 → 宽限 1.5s 后 PMU 关机。 */
static void charging_monitor_task(void *arg)
{
    (void)arg;
    const int64_t start_us = esp_timer_get_time();
    int64_t last_ui_us = 0;
    int64_t last_care_us = 0;
    int     battery_div = 0;
    bool    info_ok = false;
    battery_info_t info;
    memset(&info, 0, sizeof(info));
    s_chg_last_activity_us = start_us;

    /* 下键（GPIO61）：充电模式不初始化 button_service，这里自己配置输入上拉。
       只用于"按任意键亮屏"，亮屏状态下无动作。 */
    const gpio_config_t key_cfg = {
        .pin_bit_mask = 1ULL << CHG_DOWN_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&key_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d config failed (down key cannot wake screen)",
                 CHG_DOWN_KEY_GPIO);
    }

    while (1) {
        const int64_t now = esp_timer_get_time();

        /* 1) PMU 快照：每 200ms 一次（50ms × 4），界面仍只 1s 刷一次 */
        if (battery_div == 0) {
            info_ok = (battery_read_info(&info) == ESP_OK);
        }
        if (++battery_div >= 4) {
            battery_div = 0;
        }

        /* 电池健康保护（4.0V 软件限充）：充电模式没有 power_service，
           这里同样每秒维护一次（息屏期间也继续）。 */
        if (now - last_care_us >= (int64_t)CHG_UI_REFRESH_US) {
            last_care_us = now;
            battery_health_protection_tick();
        }

        /* 2) 上键（PMU PWRON）：
              · 按下沿 = 任意键亮屏（息屏时先亮屏）；
              · **按住满 0.6s = 进入系统**（与"按住电源键开机"语义一致）。
              PMU 的 IRQLEVEL 最短 1s，做不了 0.6s，所以这里用按住时长计时。 */
        const bool up_pressed = axp2101_is_power_key_pressed();
        if (up_pressed) {
            if (!s_chg_up_prev) {
                s_chg_up_press_us = now;
                charging_note_activity();
                if (s_chg_screen_off) {
                    charging_screen_on();
                }
            } else if (now - s_chg_up_press_us >= CHG_BOOT_HOLD_US) {
                ESP_LOGW(TAG, "charging UI: power key held 0.6s -> restart "
                              "to normal system");
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_restart();
            }
        }
        s_chg_up_prev = up_pressed;

        /* 3) 下键（GPIO61，低有效）：去抖后按下沿 = 任意键亮屏 */
        const int down_raw = (gpio_get_level(CHG_DOWN_KEY_GPIO) == 0) ? 0 : 1;
        if (down_raw != s_chg_down_stable) {
            if (++s_chg_down_debounce >= 2) {
                s_chg_down_stable = down_raw;
                s_chg_down_debounce = 0;
                if (down_raw == 0) {
                    charging_note_activity();
                    if (s_chg_screen_off) {
                        charging_screen_on();
                    }
                }
            }
        } else {
            s_chg_down_debounce = 0;
        }

        /* 4) 10 秒无按键自动息屏 */
        if (!s_chg_shutting_down && !s_chg_screen_off
                && (now - s_chg_last_activity_us) >= CHG_AUTO_SCREEN_OFF_US) {
            charging_screen_off();
        }

        /* 6) 界面刷新：只在亮屏时做（息屏期间 LVGL 已挂起） */
        if (!s_chg_shutting_down && !s_chg_screen_off && info_ok
                && (now - last_ui_us) >= (int64_t)CHG_UI_REFRESH_US) {
            last_ui_us = now;
            if (lvgl_port_lock(1000)) {
                charging_ui_update(&info);
                lvgl_port_unlock();
            }
        }

        /* 7) 拔掉充电器：宽限期后 PMU 关机（关机中/失败后不再重复触发） */
        if (!s_chg_shutting_down && info_ok && !info.vbus_present
                && (now - start_us) >= (int64_t)CHG_UNPLUG_GRACE_US) {
            charging_shutdown();
        }

        vTaskDelay(pdMS_TO_TICKS(CHG_POLL_MS));
    }
}

void boot_flow_run_charger_mode(void)
{
    s_state = BOOT_ST_CHARGING;
    lv_obj_t *root = boot_root_create();

    /* 大环展示电量：12 点方向为起点（rotation=270），顺时针填满一圈 */
    s_chg_arc = lv_arc_create(root);
    lv_obj_set_size(s_chg_arc, 330, 330);
    lv_obj_center(s_chg_arc);
    lv_obj_clear_flag(s_chg_arc, LV_OBJ_FLAG_CLICKABLE);   /* 纯显示，拖动把手也去掉 */
    lv_arc_set_rotation(s_chg_arc, 270);
    lv_arc_set_bg_angles(s_chg_arc, 0, 360);
    lv_arc_set_range(s_chg_arc, 0, 100);
    lv_arc_set_value(s_chg_arc, 0);
    lv_obj_set_style_arc_width(s_chg_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_chg_arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_chg_arc, lv_color_hex(0x2A2A2E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_chg_arc, CHG_COLOR_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_chg_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_chg_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_chg_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_chg_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_chg_arc, 0, LV_PART_KNOB);

    /* 环中心：大号百分比（montserrat_48）+ 状态文字 */
    s_chg_percent = lv_label_create(root);
    lv_label_set_text(s_chg_percent, "--%");
    lv_obj_set_style_text_font(s_chg_percent, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_chg_percent, lv_color_white(), 0);
    lv_obj_align(s_chg_percent, LV_ALIGN_CENTER, 0, -18);

    s_chg_status = lv_label_create(root);
    lv_label_set_text(s_chg_status, "正在充电");
    lv_obj_set_style_text_font(s_chg_status, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_chg_status, lv_color_white(), 0);
    lv_obj_align(s_chg_status, LV_ALIGN_CENTER, 0, 42);

    /* 最下方小字：只在电池健康保护开启时显示；关闭时整条隐藏 */
    s_chg_care = lv_label_create(root);
    lv_obj_set_style_text_font(s_chg_care, UI_SMALL_FONT, 0);
    lv_obj_set_style_text_color(s_chg_care, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(s_chg_care, LV_ALIGN_BOTTOM_MID, 0, -24);
    if (config_store_get_batt_care()) {
        lv_label_set_text(s_chg_care, "电池健康保护：开启");
    } else {
        lv_obj_add_flag(s_chg_care, LV_OBJ_FLAG_HIDDEN);
    }

    battery_info_t info;
    charging_ui_update((battery_read_info(&info) == ESP_OK) ? &info : NULL);

    if (task_create_psram_stack(charging_monitor_task, "chg_mon", CHG_MON_STACK,
                                NULL, CHG_MON_PRIO, NULL, CHG_MON_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create charging monitor task failed: charger-unplug "
                      "shutdown will NOT work");
    } else {
        ESP_LOGW(TAG, "charge-only UI shown (unplug -> PMU off, hold 0.6s -> restart)");
    }
}

/* ------------------------------------------------------------------ */
/* 扫描后台任务与进度回调                                               */
/* ------------------------------------------------------------------ */

/* 扫描进度回调（扫描任务上下文；只写 volatile 标志，绝不碰 LVGL） */
static void boot_scan_progress_cb(playlist_build_phase_t phase, uint32_t song_count,
                                  void *ctx)
{
    (void)ctx;
    s_scan_song_count = song_count;

    if (phase != PLAYLIST_BUILD_PHASE_WRITE) {
        return;
    }

    /* 进入写入阶段：只是把标志置位，由 300ms 的进度定时器（LVGL 任务上下文）
       把底部文案改成"正在写入扫描数据..."。
       ⚠ 这里**不再抢 LVGL 锁冻结界面**。flash 时代必须冻结的理由是
       `playlist_store_save_playlist` 走 esp_partition_erase/write，会关闭外部
       flash 缓存，而 LVGL 渲染要读 font 分区的 mmap 字形 → 总线卡死
       （NPF_FONT.md §7.1 的真机事故）。2026-11-16 起播放列表数据搬到 SD 卡上的
       缓存文件，写盘只是文件 I/O、**不关外部缓存**，这条事故路径从根上消失。
       ⚠ 本回调仍跑在扫描任务上下文（栈在 PSRAM），所以绝不能在里面对
       flash/分区做擦写或 map/unmap（现在也不再需要）。 */
    s_scan_writing = true;
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

    /* 若扫描成功但没有任何音乐：删掉刚生成的缓存文件，避免下次重启误判为
       "有数据"而直接进入常规界面（无音乐仍应重新提示扫描）。
       ⚠ 现在只是删卡上的文件，不关外部缓存，可以在本任务（PSRAM 栈）里直接做。 */
    if (ret == ESP_OK && stats.song_count == 0) {
        playlist_store_invalidate();
    }

    s_scan_writing = false;
    s_scan_ok = (ret == ESP_OK);
    s_scan_done = true;
    vTaskDelete(NULL);
}

/* 扫描进度定时器（LVGL 任务上下文，已持锁）：每 300ms 更新数量与阶段文案；
   扫描完成则切换界面 */
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

    /* 写入阶段：文案改成"正在写入扫描数据..."（写 SD 只要零点几秒，
       定时器可能只赶上一次，反正随后就切界面了） */
    if (s_scan_writing && !s_scan_done && s_status_label != NULL) {
        const char *cur = lv_label_get_text(s_status_label);
        if (cur == NULL || strcmp(cur, "正在写入扫描数据...") != 0) {
            lv_label_set_text(s_status_label, "正在写入扫描数据...");
        }
    }

    if (!s_scan_done) {
        return;   /* 扫描尚未结束 */
    }

    lv_timer_del(timer);   /* 停止本定时器 */
    s_scan_timer = NULL;

    if (!s_scan_ok) {
        ESP_LOGE(TAG, "scan task failed, stay on failure screen");
        boot_show_scan_failed();
        return;
    }

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
   分支（无卡 / 自动扫描 / 常规界面）都用同一份字体，不再有 TTF 时代的
   "延迟加载"。
   · 内嵌精简字库仍作为 NPF 挂载失败时的兜底（system_font.c），覆盖全部 UI
     固定文案（见 ideas_and_exps/FontSymbols.md）；
   · "数据无效"这一支会扫描 SD 卡并把结果写到卡上的缓存文件
     `/sdcard/.ecov/pldata.bin`（写 SD **不关外部缓存**，font 分区的 mmap 字形
     照常可读，所以写入期间界面不需要冻结）。 */
void boot_flow_run(void)
{
    /* 0. 低电拒启（S31）：电量 < 2% 且未在充电 → 提示屏，停在此界面。
          ⚠ 只认"电量计读数可用"的情况；PMU 读失败/未知时不拒启，
            避免把机器误锁死在提示屏上。 */
    battery_info_t binfo;
    if (battery_read_info(&binfo) == ESP_OK
            && binfo.available && binfo.percent != 0xFF
            && binfo.percent < BOOT_LOW_BAT_PERCENT
            && !binfo.vbus_present && !binfo.charging) {
        boot_show_low_battery(&binfo);
        return;
    }

    /* 1. SD 卡检查：未挂载 → 无卡界面（failsafe 重置歌曲位置） */
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        boot_show_no_sd();
        return;
    }

#if QCONF_FORCE_RESCAN
    /* 【调试】模拟设置菜单「重新扫描」的第一步：**删掉卡上缓存文件**，然后
       **继续走下面的正常流程**（is_valid() 会失败 → 自动扫描 → 写卡）。
       因此这条路径与真实重扫完全一致，只是省掉"重启"那一圈，用于没有触摸
       输入时验证；平时不启用。 */
    if (pldata_has_data()) {
        ESP_LOGW(TAG, "FORCE_RESCAN: 删除卡上缓存 %s -> %s",
                 "/sdcard/.ecov/pldata.bin",
                 esp_err_to_name(playlist_store_invalidate()));
    }
#endif

    /* 2. 播放列表缓存检查：不再只看"头部有效"，还要确认它属于**当前这张卡**
          且曲库没有增删/改名/换文件。任何一项不匹配都自动重扫，不再要求用户
          手动点"开始扫描"。
          ⚠ **完全不读 flash 里的旧 `pldata` 分区**（2026-11-16 明确取消一次性迁移）：
          卡上没缓存文件就直接扫，别再拿 flash 里的旧数据往回填——否则"重新扫描"
          （删文件 → 重启）会被旧数据瞬间"迁移"回来，扫描根本不执行。
          代价只是老固件升级后第一次开机多扫一次（9047 首约 2.3s）。 */
    bool need_scan = false;
    const char *reason = "";

    if (!playlist_store_is_valid()) {
        need_scan = true;
        reason = "no valid cache";
    } else {
        playlist_store_meta_t meta;
        if (!playlist_store_get_meta(&meta)) {
            need_scan = true;
            reason = "cache meta missing";
        } else if (meta.card_id != sd_card_get_id()) {
            need_scan = true;
            reason = "SD card changed";
            ESP_LOGW(TAG, "SD card changed: cache=%llx now=%llx",
                     (unsigned long long)meta.card_id,
                     (unsigned long long)sd_card_get_id());
            /* 换卡后旧的"上次歌曲下标/随机位置"属于旧卡，清掉以免续播到
               新卡里完全不同的一首歌；新卡从第一首开始。 */
            config_store_reset_position();
        } else {
            uint64_t sig = 0;
            uint32_t files = 0;
            int64_t t0 = esp_timer_get_time();
            esp_err_t r = playlist_builder_probe_signature(&sig, &files);
            ESP_LOGI(TAG, "library probe: %s, sig=%llx files=%u in %lld ms",
                     esp_err_to_name(r), (unsigned long long)sig,
                     (unsigned)files,
                     (long long)((esp_timer_get_time() - t0) / 1000));
            if (r != ESP_OK) {
                need_scan = true;
                reason = "library probe failed";
            } else if (sig != meta.signature || files != meta.file_count) {
                need_scan = true;
                reason = "library changed";
                ESP_LOGW(TAG, "library changed: cache sig=%llx files=%u, "
                              "now sig=%llx files=%u",
                         (unsigned long long)meta.signature,
                         (unsigned)meta.file_count,
                         (unsigned long long)sig, (unsigned)files);
            } else {
                ESP_LOGI(TAG, "playlist cache match: card=%llx files=%u",
                         (unsigned long long)meta.card_id,
                         (unsigned)meta.file_count);
            }
        }
    }

    if (need_scan) {
        ESP_LOGW(TAG, "auto scan required (%s)", reason);
        boot_show_scanning();
        return;
    }

    /* 3. 缓存与当前卡/曲库一致：直接进入常规界面 */
    boot_enter_normal();
}
