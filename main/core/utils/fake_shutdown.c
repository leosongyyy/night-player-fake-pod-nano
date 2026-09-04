#include "core/utils/fake_shutdown.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/player/player_controller.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"

static const char *TAG = "fake_shutdown";

/* 关机流程互斥标志：防止"长按伪关机进行中"又被睡眠定时器再次触发
   （deepsleep 前的停外设阶段两路并发会重复删除 I2C/SDMMC 驱动而崩溃）。
   多个任务（按键/睡眠定时器）只做"置 true"的写与判读，单字节原子。 */
static volatile bool s_shutting_down = false;

/* 关机流程（渲染提示 + 停外设 + deepsleep）调用链深（lv_refr_now 深链 +
   NVS/SD/I2C 操作），统一委托给 deep_worker 大栈任务（8KB）执行——曾直接在
   按键任务（栈 3KB）里跑导致栈溢出重启（A stack overflow in task
   button_svc），故不再自建独立关机任务，复用 deep_worker 执行器。 */

/* ------------------------------------------------------------------ */
/* 关机提示 UI（LVGL 顶层覆盖层）                                      */
/* ------------------------------------------------------------------ */

/* 阶段 1（保存中）：黑屏居中一行白字 */
#define FS_SAVING_TEXT      "正在保存配置"

/* 阶段 2（定格提示，等待电源芯片断电/按键唤醒）：黑色全屏提示框 +
   1px 黄色外框（30px 圆角，距屏幕边缘 10px），内部按"上图标 / 中主消息 /
   下按键行"分布排版：
      （大号 LV_SYMBOL_POWER，黄，靠上）
      系统数据已保存                          ← 白色，中部第一行
      您现在可以安全断开电源了                ← 白色，中部第二行
      ↑ 继续按住上键以关机                     ← 灰色（LV_SYMBOL_UP），靠下
      ↓ 单击下键可重新启动                     ← 灰色（LV_SYMBOL_DOWN） */
#define FS_PANEL_MARGIN     10                 /* 黄框距屏幕边缘 */
#define FS_PANEL_X          FS_PANEL_MARGIN
#define FS_PANEL_Y          FS_PANEL_MARGIN
#define FS_PANEL_W          (EXAMPLE_LCD_H_RES - 2 * FS_PANEL_MARGIN)
#define FS_PANEL_H          (EXAMPLE_LCD_V_RES - 2 * FS_PANEL_MARGIN)
#define FS_PANEL_RADIUS     18                 /* 黄色外框圆角 */
#define FS_PANEL_BG         lv_color_black()   /* 边框内为黑色 */
#define FS_PANEL_BORDER     lv_color_hex(0xFFC000)   /* 黄，与按压高亮同色系 */
#define FS_BORDER_WIDTH     1                  /* 外框 1px */

#define FS_TEXT_MAIN        lv_color_white()
#define FS_TEXT_SUB         lv_color_hex(0x9E9E9E)   /* 灰色辅助行 */
#define FS_ICON_COLOR       lv_color_hex(0xFFC000)

/* 框内排版定位 */
#define FS_ICON_TOP         40                 /* 图标距框顶（"靠上"） */
#define FS_MAIN_GAP         12                 /* 两行主消息行间距 */
/* 主消息两行（各一行高 33）整体在框内垂直居中（"中间是主提示"） */
#define FS_MAIN_TOP         ((FS_PANEL_H - (33 * 2 + FS_MAIN_GAP)) / 2)
#define FS_ROW_BOTTOM       32                 /* 最下一行距框底 */
#define FS_ROW_GAP          10                 /* 两行按键行间距 */

/* 全屏提示覆盖层（黑底）：显示在 LVGL 顶层，覆盖当前界面；
   A 键长按关机时把提示定格为最后一帧，等待硬件断电自然熄灭。 */
static lv_obj_t *s_overlay = NULL;

/* 取覆盖层（首次创建），并清空上次内容，返回可直接放子控件的容器 */
static lv_obj_t *fake_shutdown_overlay_reset(void)
{
    if (s_overlay == NULL) {
        s_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_overlay, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_overlay, 0, 0);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
    }
    lv_obj_clean(s_overlay);
    return s_overlay;
}

/* 阶段 1：居中显示「正在保存配置」（黑底白字） */
static void fake_shutdown_show_saving(void)
{
    lv_obj_t *ov = fake_shutdown_overlay_reset();
    lv_obj_t *lbl = lv_label_create(ov);
    lv_label_set_text(lbl, FS_SAVING_TEXT);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    lv_refr_now(NULL);   /* 强制刷出（须持锁） */
}

/* 面板内"图标 + 文字"一行（灰色，水平居中，用于 UP/DOWN 两行说明）。
   行宽撑满面板、高度固定为文本行高（33），图标与文字在行内垂直居中，
   返回行句柄供调用方定位 */
static lv_obj_t *fake_shutdown_final_row(lv_obj_t *panel, const char *icon,
                                         const char *text)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 33);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ic, FS_TEXT_SUB, 0);

    lv_obj_t *tx = lv_label_create(row);
    lv_label_set_text(tx, text);
    lv_obj_set_style_text_font(tx, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(tx, FS_TEXT_SUB, 0);
    return row;
}

/* 阶段 2：定格提示（黑色全屏框 + 1px 黄边框，四边距屏幕 10px）。
   框内排版：上方大号电源图标、中部白色主句、下方两行灰色按键提示 */
static void fake_shutdown_show_final(void)
{
    lv_obj_t *ov = fake_shutdown_overlay_reset();

    /* 全屏提示框：黑底、1px 黄色外框，距屏幕边缘 FS_PANEL_MARGIN */
    lv_obj_t *panel = lv_obj_create(ov);
    lv_obj_set_size(panel, FS_PANEL_W, FS_PANEL_H);
    lv_obj_set_pos(panel, FS_PANEL_X, FS_PANEL_Y);
    lv_obj_set_style_bg_color(panel, FS_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, FS_PANEL_RADIUS, 0);
    lv_obj_set_style_border_width(panel, FS_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(panel, FS_PANEL_BORDER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    /* 大号电源图标（黄色，靠上） */
    lv_obj_t *power = lv_label_create(panel);
    lv_label_set_text(power, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(power, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(power, FS_ICON_COLOR, 0);

    /* 白色主消息（框内中部，两行）：第一行「系统数据已保存」，
       第二行「您现在可以安全断开电源了」；各自水平居中（超宽折行兜底） */
    lv_obj_t *main_l1 = lv_label_create(panel);
    lv_label_set_text(main_l1, "系统数据已保存");
    lv_obj_set_width(main_l1, FS_PANEL_W);
    lv_obj_set_style_text_font(main_l1, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(main_l1, FS_TEXT_MAIN, 0);
    lv_obj_set_style_text_align(main_l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(main_l1, LV_LABEL_LONG_WRAP);

    lv_obj_t *main_l2 = lv_label_create(panel);
    lv_label_set_text(main_l2, "您现在可以安全断开电源了");
    lv_obj_set_width(main_l2, FS_PANEL_W);
    lv_obj_set_style_text_font(main_l2, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(main_l2, FS_TEXT_MAIN, 0);
    lv_obj_set_style_text_align(main_l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(main_l2, LV_LABEL_LONG_WRAP);

    /* 灰色辅助行（靠下）：上键关机 / 下键唤醒重启 */
    lv_obj_t *row_up = fake_shutdown_final_row(panel, LV_SYMBOL_UP,
                                               "继续按住上键以关机");
    lv_obj_t *row_down = fake_shutdown_final_row(panel, LV_SYMBOL_DOWN,
                                                 "单击下键可重新启动");

    /* 先整体布局一次，让主消息（折行）等自动尺寸就绪，再按上/中/下定位 */
    lv_obj_update_layout(panel);
    lv_obj_align(power, LV_ALIGN_TOP_MID, 0, FS_ICON_TOP);
    lv_obj_align(main_l1, LV_ALIGN_TOP_MID, 0, FS_MAIN_TOP);
    lv_obj_align(main_l2, LV_ALIGN_TOP_MID, 0, FS_MAIN_TOP + 33 + FS_MAIN_GAP);
    lv_obj_align(row_up, LV_ALIGN_BOTTOM_MID, 0,
                 -(FS_ROW_BOTTOM + 33 + FS_ROW_GAP));   /* UP 行（上一条） */
    lv_obj_align(row_down, LV_ALIGN_BOTTOM_MID, 0, -FS_ROW_BOTTOM);

    lv_refr_now(NULL);   /* 强制刷出（须持锁），保证定格前提示完整显示 */
}

/* 统一的伪关机流程（进入 deepsleep，不返回）：
   - show_hint:    是否显示两阶段提示（A 键长按关机时显示）
   - keep_display: 是否保留屏幕画面（true = 不主动关屏，靠断电自然熄灭；
                   false = 主动关屏，普通省电伪关机） */
static void fake_shutdown_common(bool show_hint, bool keep_display)
{
    /* 0. 显示「正在保存配置」，随后保存当前播放位置到 NVS
         （关机后开机自动恢复播放进度）。先保存再做停播/关外设，
         因为停播后播放状态即失效。 */
    if (show_hint) {
        if (lvgl_port_lock(-1)) {
            fake_shutdown_show_saving();
            lvgl_port_unlock();
        }
    }
    player_controller_save_last_position();

    /* 1. 停止播放并等待解码任务退出（释放文件句柄与 I2S） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_IDLE && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
    }

    /* 2. 保存完成，显示最终提示并定格（A 键长按关机时）：
         提示用户"继续按住上键（A/电源键）满 3s 由关机芯片彻底断电，
         或单击下键（B 键）唤醒重新启动"；随后挂起 LVGL 定格画面。 */
    if (show_hint) {
        if (lvgl_port_lock(-1)) {
            fake_shutdown_show_final();
            lvgl_port_unlock();
        }
    }

    /* 3. 挂起 LVGL 任务（停止刷屏与触摸轮询，避免后面删除 I2C 驱动后报错）。
         画面定格在最后一帧：AMOLED 为存储型显示，保持供电即持续显示提示。 */
    lvgl_port_suspend();

    /* 4. 复位 CS43131（GPIO41 拉低硬件复位：全部电源域含晶振一起掉电，
           最彻底关机且状态确定；唤醒后 audio_init → cs43131_init 会重新
           走完整复位+配置流程恢复） */
    cs43131_reset_for_sleep();

    /* 5. 关闭显示屏（普通伪关机主动关屏省电）。
         A 键长按关机时不关屏：保留定格提示，等待电源侧关机芯片在长按
         满 3s 时断电，屏幕靠断电自然熄灭（主动关屏会让用户分不清
         是否真的关机）。 */
    if (!keep_display) {
        display_power_off();
    }

    /* 6. 关闭触摸控制器并释放 I2C 总线驱动 */
    touch_power_off();

    /* 7. 卸载 SD 卡并释放 SDMMC 主机 */
    sd_card_deinit();

    /* 8. 配置 deep sleep 唤醒：B 键（GPIO0，RTC GPIO）按下（低电平）唤醒。
         关键约束（ESP32-S3）：
        - deep sleep GPIO 唤醒只支持 RTC GPIO（GPIO0~21，SOC_RTCIO_PIN_COUNT=22），
          A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒——A 键只负责
          长按触发伪关机，真正断电由电源侧关机芯片硬件完成，无需唤醒；
        - esp_sleep_enable_gpio_wakeup 仅支持 light sleep（IDF 文档明确），
          此前误用导致进入 deepsleep 后快速误唤醒；
        - RTC 外设关闭时内部上拉不工作，需外部电阻——GPIO0 的板载 10k 上拉
          正好满足，平时高电平、按下拉低触发唤醒。 */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW));

    ESP_LOGI(TAG, "deep sleep: peripherals off, press B key (GPIO0) to wake");
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等待日志刷出 */
    esp_deep_sleep_start();
}

/* 关机流程执行体（在 deep_worker 大栈任务上下文运行）：arg != NULL =
   A 键长按关机（显示提示、不关屏）；arg == NULL = 普通伪关机（无提示、
   主动关屏）。进入 deepsleep 不返回（deep_worker 任务随之随芯片睡眠，
   唤醒即系统重启，无需恢复）。 */
static void fake_shutdown_run(void *arg)
{
    const bool power_key = (arg != NULL);
    fake_shutdown_common(power_key, power_key);
}

void fake_shutdown_enter(void)
{
    /* 防御：伪关机流程已在进行（长按关机期间又触发睡眠定时器）时
       直接返回，避免并发关闭外设重复删除驱动 */
    if (s_shutting_down) {
        return;
    }
    s_shutting_down = true;
    ESP_LOGI(TAG, "=== fake shutdown: close all peripherals, enter deep sleep ===");
    /* 委托 deep_worker 大栈任务执行关机流程（本调用方只做浅投递）；
       失败（executor 未初始化/卡死）时复位标志，允许稍后重试 */
    if (!deep_worker_call(fake_shutdown_run, NULL)) {
        s_shutting_down = false;
    }
}

void fake_shutdown_wait_power_off(void)
{
    if (s_shutting_down) {
        return;
    }
    s_shutting_down = true;
    ESP_LOGI(TAG, "=== power-key shutdown: save, freeze hint, wait hardware power off ===");
    /* 同上：深链交给 deep_worker 执行 */
    if (!deep_worker_call(fake_shutdown_run, (void *)1)) {
        s_shutting_down = false;
    }
}
