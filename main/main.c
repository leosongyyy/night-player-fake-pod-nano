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
#include "core/hardware/boot_reason.h"
#include "core/hardware/button_service.h"
#include "core/hardware/display.h"
#include "core/hardware/pldata.h"
#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/fonts/system_font.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
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

    /* 系统字体（必须早于任何 UI 创建，含开机封面上的提示文字）：
       先试 font 分区的 NPF1 全字库（mmap + 填指针，零解析），失败则回退内嵌
       精简字库（system_global_font_lite）。位图字体不像 FreeType 那样有"字形
       缓存占 PSRAM / 关缓存期间取不到字形"的问题，所以直接在这里挂载，不需要
       像 TTF 时代那样延迟到 boot_enter_normal。
       ⚠ 放在 LVGL 锁外调用：它只做分区查找 + mmap + 填结构体，不碰 LVGL 对象。 */
    ESP_LOGI(TAG, "Initialize system font");
    system_font_init();
    mem_trace_log("04b-system_font");

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

    ESP_LOGI(TAG, "Initialize AXP2101 PMU (battery voltage / SOC / charge status)");
    battery_init();
    mem_trace_log("07-battery");

    /* 开机原因：主控复位/深度睡眠唤醒原因 + PMU 本次开机来源
       （按键开机 / 插电开机 / 电池插入…）。⚠ 必须在 battery_init 之后：
       要读 AXP2101 的 0x20/0x21。 */
    boot_reason_init();

    /* 电池健康保护（NVS 配置 → 4.0V 软件限充）：PMU 每次上电都会把配置
       恢复成 EFUSE 默认，所以每次开机都要重新下发；关闭时也会恢复充电。
       ⚠ 不能把 CV 降到 4.0V：电池高于 CV 时 PMU 会断开 BATFET，拔电会硬断电。 */
    battery_set_health_protection(config_store_get_batt_care());

    /* 插充电器自动开机（PMU 0x20 bit2 置位 + 主控"上电类"复位）→ 只进
       "正在充电"专用界面：不初始化 RTC / 音频 / 播放列表 / SD / 按键 / 电源
       服务，既省电也不会在充电时放歌。app_main 到此返回，LVGL 任务与充电
       监控任务继续工作（拔电 → PMU 关机；按住上键 0.6s → esp_restart 进系统）。 */
    if (boot_reason_is_charger_boot()) {
        ESP_LOGW(TAG, "charger boot: enter charge-only UI");
        if (lvgl_port_lock(-1)) {
            boot_flow_run_charger_mode();
            lv_refr_now(NULL);

            /* 充电界面至少半亮：配置亮度太低（16~127）时看起来像没开机 */
            uint8_t chg_brightness = config_store_get_brightness();
            if (chg_brightness < 128) {
                chg_brightness = 128;
            }
            ESP_LOGI(TAG, "Turn on display (charging UI, brightness %d)",
                     chg_brightness);
            display_set_brightness(chg_brightness);
            lvgl_port_unlock();
        }
        mem_trace_log("07c-charging_ui");
        return;
    }

    /* PCF85063A 实时时钟：必须在 touch_init() 之后（复用触摸建立的 I²C0 总线）。
       ⚠ 只在时间无效（OS=1/出厂默认/字段非法）时用"项目诞生日"校时，
         有效时不覆盖备用电源保住的走时。失败不致命（时间显示降级为 --:--）。 */
    ESP_LOGI(TAG, "Initialize PCF85063A RTC");
    if (rtc_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC unavailable (time display disabled)");
    }
    mem_trace_log("07b-rtc");

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
    // 真正的关机由 AXP2101 负责（驱动已接入，关机流程待接）)
    sleep_timer_init();
    mem_trace_log("13-sleep_timer");

    // 开机自动开启睡眠定时（config_store 的 sleep_auto，默认"总是关闭"）：
    // 三档 —— 总是关闭 / 按时间段（当前时刻落在时间段内才开）/ 总是开启。
    // 判定收敛到 sleep_timer_boot_autostart_min（与 boot_flow 的"定时已开启"
    // Toast 提醒共用同一判定，见 FUTURE_PLAN 第 27 点）。
    // ⚠ 模式为"总是关闭"或"按时间段但当前不在区间内"时，开机一律不启动定时——
    //   全局记忆的定时时长（sleep_default_min）只是"下次开启时用的默认档位"，
    //   不是开机就要生效的指令。历史 bug：这里曾在 else 分支按记忆时长恢复定时，
    //   导致关掉开关后每次开机定时器照样自动开启。
    //   睡眠定时与配置槽位互相独立（时长/模式/时间段都是全局键，切槽位不重开表）。
    uint32_t boot_sleep_min = sleep_timer_boot_autostart_min();
    if (boot_sleep_min > 0) {
        sleep_timer_set(boot_sleep_min);
        ESP_LOGI(TAG, "boot: sleep timer auto started (%u min)",
                 (unsigned)boot_sleep_min);
    } else {
        ESP_LOGI(TAG, "boot: sleep timer off (auto mode=%d, range=%02u-%02u)",
                 (int)config_store_get_sleep_auto_mode(),
                 (unsigned)config_store_get_sleep_range_start_hour(),
                 (unsigned)config_store_get_sleep_range_end_hour());
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

    // 启动性能监视（内存 + 双核 CPU 使用率），Kconfig CONFIG_PERFORMANCE_MONITOR 控制
#ifdef CONFIG_PERFORMANCE_MONITOR
    startPerformanceMonitor();
    mem_trace_log("17-perf_monitor");
#endif
}
