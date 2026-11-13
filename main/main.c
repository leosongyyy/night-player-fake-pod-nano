#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/eq.h"
#include "core/hardware/battery.h"
#include "core/hardware/button_service.h"
#include "core/hardware/display.h"
#include "core/hardware/pldata.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/fonts/font_ttf.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/battery_logger.h"
#include "core/utils/deep_worker.h"
#include "core/utils/mem_trace.h"
#include "core/utils/performance_monitor.h"
#include "core/utils/power_service.h"
#include "core/utils/sleep_timer.h"
#include "ui/boot_flow.h"
#include "ui/key_handler.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/* 按键事件：亮屏/息屏两套映射由 ui/key_handler 统一分发                 */
/* （亮屏：A=音量+ B=音量- A长按=伪关机 B长按=音量-10 双按=息屏；          */
/*   息屏：A=播放/暂停 B=下一曲 B长按=上一曲 A长按=亮屏+伪关机 双按=亮屏）  */
/* ------------------------------------------------------------------ */

/* 触摸初始化完成回调（触摸异步初始化任务上下文）：
   把就绪的触摸句柄交给 LVGL 输入设备，触摸输入即刻生效 */
static void on_touch_ready(esp_lcd_touch_handle_t tp, void *ctx)
{
    (void)ctx;
    lvgl_port_set_touch_handle(tp);
    ESP_LOGI(TAG, "Touch ready, LVGL touch input enabled");
}

void app_main(void)
{
    /* S31 平台不再使用固件伪关机（deep sleep）：关机/唤醒由 AXP2101 负责，
       尚未适配。此处只保留冷启动日志。 */
    ESP_LOGI(TAG, "boot: cold start (S31, no firmware deep-sleep shutdown)");

    // 用户配置存储（NVS）：尽早初始化，开机恢复音量/亮度/循环模式等
    ESP_LOGI(TAG, "Initialize config store (NVS)");
    config_store_init();
    /* 配置槽位（NightPlayer「配置插槽」）：把"当前活跃配置"整份载入内存。
       ⚠ 必须紧跟 config_store_init 且**早于任何读取可切换字段的动作**：
       player_controller_init 要按 last_song / loop / shuffle 恢复现场，
       下面还要按它应用音量与 EQ。空槽位/首次升级无槽位记录时保持旧全局键值
       （见 core/storage/config_store.h 与 ideas_and_exps/CONFIG_SLOTS.md）。 */
    config_store_load_active_slot();
    mem_trace_log("01-config_store");

    /* 深路径执行器（常驻大栈任务，Core0）：息屏/亮屏/伪关机的深调用链
       （esp_pm_configure + AMOLED 重初始化 + lv_refr_now 全屏重刷 + 停外设）
       与"重新扫描"的关缓存段集中到该任务执行，业务任务只做浅投递（见
       core/utils/deep_worker.h）。
       ⚠ 必须在**开机早期**创建：内部 RAM 在开机末期高度碎片化
       （最大连续块约 8.6KB），8KB 内部栈此时才稳；曾因把它放在 boot_flow
       之后且栈加到 12KB，导致任务创建静默失败 → 每次深调用等满 5s 超时
       （按键息屏/亮屏延迟 5 秒）。 */
    ESP_LOGI(TAG, "Initialize deep worker");
    deep_worker_init();
    mem_trace_log("02-deep_worker");

    // 硬件初始化
    ESP_LOGI(TAG, "Initialize display");
    esp_lcd_panel_handle_t panel_handle = display_init();
    mem_trace_log("03-display");

    /* LVGL 移植层（显示驱动注册 + tick + LVGL 任务 + 触摸输入设备）
       ⚠ 提前到 display_init 之后**立刻**启动（不再等其他硬件）：它只依赖
       面板句柄（display_init 返回）与 config_store（读"触摸测试"开关），
       不依赖 touch/audio/pldata/SD——触摸本就是异步初始化，此处句柄传 NULL，
       就绪后由 on_touch_ready 经 lvgl_port_set_touch_handle 交付。
       提前启动的目的：随后立刻画一张默认封面（boot_splash_show）并点亮屏幕，
       把 touch/audio/pldata/button/SD 与主界面创建（app_ui_create 实测约
       700ms）的耗时全部藏到封面背后——实测首次点亮从约 1.9s 提前到约 0.7s，
       而"进入主界面"的时刻不变。 */
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_init(panel_handle, NULL);
    mem_trace_log("04-lvgl_port");

    /* 系统字体（TTF / FreeType）**故意不在这里加载**：改成**延迟加载**，
    由 boot_flow 在"确实要进常规界面"时才调 font_ttf_init()（见 ui/boot_flow.c
    的 boot_enter_normal）。好处：
      · 低电 / 无卡 / 待扫描这些非正常启动分支全程用内嵌位图字体，boot 阶段
        完全不碰 FreeType、不占 PSRAM 字形缓存；
      · "待扫描"分支紧接着要扫描 SD 卡并整体重写 pldata（约占 2.8MB PSRAM，
        写 flash 时还会关外部内存缓存、PSRAM 不可访问），少一层 TTF 依赖就少
        一处会踩"关缓存陷阱"的地方；
      · 内嵌位图字库已覆盖全部 UI 文案（见 ideas_and_exps/FontSymbols.md），
        这些只有固定文案的界面不会缺字。
    ⚠ 应用侧只调 lv_freetype_font_create()，不要再调 lv_freetype_init()
      （lv_init() 已经初始化过 FreeType 引擎）。 */

    /* 开机封面（默认封面）：此时面板亮度仍为 0，先把整屏写满再点亮——与原
       来"全黑启动防白屏"的顺序完全一致。
       ⚠ 亮度命令与 LVGL 刷屏共用 SPI 总线，必须与首帧渲染在同一个 LVGL 锁内
       连续执行；锁外执行会被更高优先级的 LVGL 任务抢占并发刷屏，导致画面
       定格/花屏。 */
    if (lvgl_port_lock(-1)) {
        boot_splash_show();
        lv_refr_now(NULL);

        uint8_t splash_brightness = config_store_get_brightness();
        ESP_LOGI(TAG, "Turn on display (boot splash, brightness %d)", splash_brightness);
        display_set_brightness(splash_brightness);
        lvgl_port_unlock();
    }
    mem_trace_log("05-boot_splash");

    ESP_LOGI(TAG, "Initialize touch");
    /* 触摸异步初始化：立即返回（就绪前为 NULL）；就绪后经 ready 回调把句柄交给
       已在 lvgl_port_init 注册好的 LVGL 输入设备（就绪前触摸回调不给数据），
       故此处不需要返回值 */
    touch_init();
    /* 触摸就绪回调：把句柄交给 LVGL 输入设备（就绪前触摸回调不给数据） */
    touch_register_ready_cb(on_touch_ready, NULL);
    mem_trace_log("06-touch");

    ESP_LOGI(TAG, "Initialize battery (S31 placeholder, AXP2101 TBD)");
    battery_init();
    mem_trace_log("07-battery");

    ESP_LOGI(TAG, "Initialize audio (I2S + CS43131)");
    audio_init();
    mem_trace_log("08-audio");

    /* 应用用户音量配置（NVS 持久化，档位 0~30；audio_init 内部先按默认档初始化 DAC） */
    audio_set_volume_level(config_store_get_volume());

    /* 恢复用户 EQ 预设（NVS 持久化，FUTURE_PLAN 第 10 点）：参数数据已随
       文件名存入 NVS，直接应用 DSP 状态（未配置采样率时只保存参数，
       首次播放 audio_pcm_start → audio_configure_fs 时生成滤波器系数）；
       无记录则 EQ 保持默认关闭 */
    {
        char eq_name[EQ_FILE_NAME_MAX];
        eq_preset_t eq_preset;
        if (config_store_get_eq(eq_name, sizeof(eq_name), &eq_preset)) {
            eq_apply(&eq_preset);
            ESP_LOGI(TAG, "Restore EQ preset: %s (%u bands)", eq_name,
                     (unsigned)eq_preset.band_count);
        }
    }

    ESP_LOGI(TAG, "Initialize pldata partition (播放列表数据)");
    if (pldata_init() != ESP_OK) {
        ESP_LOGW(TAG, "pldata partition unavailable (播放列表持久化不可用)");
    }
    mem_trace_log("09-pldata");

    ESP_LOGI(TAG, "Initialize button service (single key=GPIO61)");
    button_service_init();
    mem_trace_log("10-button");
    /* 按键分发（亮屏/息屏两套映射）在 LVGL 初始化后统一注册（见下） */

    ESP_LOGI(TAG, "Initialize SD card (SDMMC)");
    sd_card_init();   // 挂载失败不阻塞，由 boot_flow 依据 sd_card_is_mounted() 决定流程
    mem_trace_log("11-sd");

    // 开机流程：电池电压 / SD 卡 / 播放列表数据检查，决定进入哪个界面。
    // （LVGL API 非线程安全，需持锁操作；本函数只创建首屏并启动后台任务，不阻塞；
    //   任一分支都会经 boot_root_create() 删除开机封面，替换为对应界面）
    ESP_LOGI(TAG, "Run boot flow");
    if (lvgl_port_lock(-1)) {
        boot_flow_run();
        // 强制同步渲染：把"封面 → 目标界面"的切换在同一个锁内一次提交，避免
        // 中间出现半成品帧（全程持锁，LVGL 任务无法插入渲染）
        lv_refr_now(NULL);

        // 复核亮度（用户配置值；无卡界面会在 boot_flow 内把它拉满到 255 并写回
        // NVS，低电警告屏则依赖此处点亮）：与上面渲染在同一个 LVGL 锁内连续执行。
        // ⚠ 亮度命令与 LVGL 刷屏共用 SPI 总线，必须在持锁时调用；锁外执行会被
        // 更高优先级的 LVGL 任务抢占并发刷屏，导致画面定格/花屏。
        uint8_t boot_brightness = config_store_get_brightness();
        ESP_LOGI(TAG, "Turn on display (restore brightness %d)", boot_brightness);
        display_set_brightness(boot_brightness);
        lvgl_port_unlock();
    }
    mem_trace_log("12-boot_flow+first_frame");

    ESP_LOGI(TAG, "App started");

    // 深路径执行器已在 app_main 早期初始化（见上：内部 RAM 碎片化前创建）

    // 睡眠定时器（默认关闭，由播放设置页开启；到点暂停播放 + 息屏，
    // S31 的真正关机由 AXP2101 负责、尚未适配）
    sleep_timer_init();
    mem_trace_log("13-sleep_timer");

    // 开机默认开启睡眠定时（config_store 的 sleep_def_on，默认关）：
    // 开启时用默认时长（sleep_default_min，默认 45 分钟）自动启动定时。
    // 判定收敛到 sleep_timer_boot_autostart_min（与 boot_flow 的"定时已开启"
    // Toast 提醒共用同一判定，见 FUTURE_PLAN 第 27 点）。
    uint32_t boot_sleep_min = sleep_timer_boot_autostart_min();
    if (boot_sleep_min > 0) {
        sleep_timer_set(boot_sleep_min);
        ESP_LOGI(TAG, "boot: sleep timer auto started (%u min)",
                 (unsigned)boot_sleep_min);
    } else {
        /* 配置槽位：关掉"开机默认开启"时，仍按**当前配置里存的定时时长**恢复
           定时（每个配置各自保存自己的定时时长，见 CONFIG_SLOTS.md）。这里
           在 sleep_timer_init 之后调用，故 `sleep_timer_set` 会真正建任务。 */
        uint32_t slot_sleep_min = config_store_get_sleep_default_min();
        if (slot_sleep_min > 0) {
            sleep_timer_set(slot_sleep_min);
            ESP_LOGI(TAG, "boot: sleep timer restored from config slot (%u min)",
                     (unsigned)slot_sleep_min);
        }
    }

    // 按键分发（S31 单键：亮屏=音量-；息屏=亮屏+切歌）。
    // 放在 LVGL 初始化之后，注册 button_service 事件回调。
    key_handler_init();
    mem_trace_log("14-key_handler");

    // 电源服务（power_service）：esp_pm 固定频率（亮屏 320MHz/息屏 80MHz）+
    // 触摸活动订阅 + 无操作超时自动息屏（默认 30 秒，设置页可调）。
    // 低电/暂停自动关机随 S31 迁移移除（关机交给 AXP2101）。需在 LVGL 之后。
    power_service_init();
    mem_trace_log("15-power_service");

    // 电池电压采样日志（S31 占位：AXP2101 尚未适配，函数只打印说明不建任务）
    battery_logger_init();
    mem_trace_log("16-batt_logger");

    // 启动性能监视（内存 + 双核 CPU 使用率），Kconfig CONFIG_PERFORMANCE_MONITOR 控制
#ifdef CONFIG_PERFORMANCE_MONITOR
    startPerformanceMonitor();
    mem_trace_log("17-perf_monitor");
#endif
}
