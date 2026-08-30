#include "ui/main_ui.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"   /* EXAMPLE_LCD_H_RES（屏幕宽度） */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "main_ui";

/* 当前曲目显示名（单曲，44.1kHz；LVGL 内置字体无中文字形，用英文） */
#define TRACK_NAME "The Still Sea-Cliff"

static lv_obj_t *play_btn_label = NULL;
static lv_obj_t *track_label = NULL;
static lv_obj_t *dac_status_label = NULL;
static lv_obj_t *progress_slider = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *vol_label = NULL;

/* 进度条是否正被手指拖动（拖动中不刷新进度，松手才 seek） */
static volatile bool s_dragging = false;

/* ------------------------------------------------------------------ */
/* CS43131 电源测试（关机 = 关闭大部分电路，仅保留 I2C 控制接口）          */
/* ------------------------------------------------------------------ */

/* 确保 CS43131 已开机：若处于关机状态则先开机。
   返回 true 表示 DAC 可用。播放/恢复播放前调用（UI 层，仅测试用）。 */
static bool ensure_dac_powered(void)
{
    if (!cs43131_is_powered_down()) {
        return true;
    }
    ESP_LOGI(TAG, "CS43131 is powered down, powering up...");
    if (cs43131_power_up() != ESP_OK) {
        ESP_LOGE(TAG, "CS43131 power up failed");
        return false;
    }
    return true;
}

/* 刷新 DAC 状态标签（开机/关机） */
static void dac_status_update(void)
{
    if (cs43131_is_powered_down()) {
        lv_label_set_text(dac_status_label, "DAC: OFF");
        lv_obj_set_style_text_color(dac_status_label, lv_palette_main(LV_PALETTE_RED), 0);
    } else {
        lv_label_set_text(dac_status_label, "DAC: ON");
        lv_obj_set_style_text_color(dac_status_label, lv_palette_main(LV_PALETTE_GREEN), 0);
    }
}

/* 关机测试按钮：播放中先暂停（等待播放任务真正停 I2S 并静音），再关闭 CS43131。
   对应需求：播放途中关闭 CS43131 时，先暂停播放再关闭（UI 层，仅测试）。 */
static void dac_power_off_cb(lv_event_t *e)
{
    (void)e;
    if (mp3_player_get_state() == MP3_STATE_PLAYING) {
        mp3_player_pause();
        /* 等待播放任务完成暂停（内部会停 I2S + 静音 DAC），保证"先暂停后关闭" */
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_PAUSED && wait < 500) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
        if (mp3_player_get_state() != MP3_STATE_PAUSED) {
            ESP_LOGW(TAG, "pause timeout, still powering down");
        }
    }
    if (cs43131_power_down() != ESP_OK) {
        ESP_LOGE(TAG, "CS43131 power down failed");
    }
    dac_status_update();
}

/* 开机测试按钮：仅开机并恢复音量（恢复播放由播放按钮负责，其内部会检查 DAC 状态） */
static void dac_power_on_cb(lv_event_t *e)
{
    (void)e;
    ensure_dac_powered();
    dac_status_update();
}

/* ------------------------------------------------------------------ */
/* 播放 / 暂停按钮                                                       */
/* ------------------------------------------------------------------ */

static void play_pause_btn_cb(lv_event_t *e)
{
    (void)e;
    mp3_player_state_t st = mp3_player_get_state();
    if (st == MP3_STATE_PLAYING) {
        mp3_player_pause();
        return;
    }
    /* 继续播放前确保 CS43131 已开机：关机状态下先开机，再恢复/启动播放
       （对应需求：继续播放时检查 CS43131，未开机则先开启再恢复；UI 层，仅测试） */
    if (!ensure_dac_powered()) {
        ESP_LOGE(TAG, "DAC unavailable, cannot play");
        return;
    }
    if (st == MP3_STATE_PAUSED) {
        mp3_player_resume();
    } else {
        /* 停止/播放结束后，重新播放当前曲目 */
        mp3_player_play(TRACK_44100_PATH);
    }
    /* 按钮文字由进度定时器统一刷新 */
}

/* ------------------------------------------------------------------ */
/* 进度条拖动（seek）                                                    */
/* ------------------------------------------------------------------ */

static void progress_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_dragging = true;
}

static void progress_released_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t target = lv_slider_get_value(slider);
    s_dragging = false;

    /* seek 会使暂停中的播放恢复（播放任务内部 audio_pcm_start），
       恢复前同样确保 DAC 已开机（仅测试用） */
    if (mp3_player_get_state() != MP3_STATE_IDLE && !ensure_dac_powered()) {
        ESP_LOGE(TAG, "DAC unavailable, seek skipped");
        return;
    }
    mp3_player_seek((uint32_t)target);
}

/* ------------------------------------------------------------------ */
/* 进度刷新定时器（每 500ms）                                            */
/* ------------------------------------------------------------------ */

static void progress_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    mp3_player_state_t st = mp3_player_get_state();
    uint32_t pos = mp3_player_get_position_s();
    uint32_t dur = mp3_player_get_duration_s();

    /* DAC 状态标签（开机/关机，随电源操作实时刷新） */
    dac_status_update();

    /* 进度条 range + 时间标签 */
    if (dur > 0) {
        lv_slider_set_range(progress_slider, 0, (int32_t)dur);
        lv_label_set_text_fmt(time_label, "%02u:%02u / %02u:%02u",
                              (unsigned)(pos / 60), (unsigned)(pos % 60),
                              (unsigned)(dur / 60), (unsigned)(dur % 60));
    } else {
        lv_slider_set_range(progress_slider, 0, 1);
        lv_label_set_text(time_label, "--:-- / --:--");
    }
    /* 拖动中不覆盖用户手指位置 */
    if (!s_dragging) {
        lv_slider_set_value(progress_slider, (int32_t)pos, LV_ANIM_OFF);
    }

    /* 按钮文字随状态切换 */
    if (st == MP3_STATE_PLAYING) {
        lv_label_set_text(play_btn_label, LV_SYMBOL_PAUSE " 暂停");
    } else if (st == MP3_STATE_PAUSED) {
        lv_label_set_text(play_btn_label, LV_SYMBOL_PLAY " 播放");
    } else {
        lv_label_set_text(play_btn_label, LV_SYMBOL_PLAY " 播放");
    }
}

/* ------------------------------------------------------------------ */
/* 音量滑块                                                             */
/* ------------------------------------------------------------------ */

static void volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t vol = (uint8_t)lv_slider_get_value(slider);
    audio_set_volume(vol);
    lv_label_set_text_fmt(vol_label, LV_SYMBOL_VOLUME_MAX " %u%%", vol);
}

/* ------------------------------------------------------------------ */
/* UI 组装                                                              */
/* ------------------------------------------------------------------ */

void main_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_AUDIO " Now Playing");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

    /* 歌曲名 */
    track_label = lv_label_create(scr);
    lv_label_set_text(track_label, TRACK_NAME);
    lv_obj_set_style_text_font(track_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(track_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(track_label, LV_ALIGN_TOP_MID, 0, 102);

    /* 播放 / 暂停按钮 */
    lv_obj_t *play_btn = lv_btn_create(scr);
    lv_obj_set_size(play_btn, 200, 66);
    lv_obj_align(play_btn, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_color(play_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_shadow_width(play_btn, 4, 0);
    lv_obj_add_event_cb(play_btn, play_pause_btn_cb, LV_EVENT_CLICKED, NULL);

    play_btn_label = lv_label_create(play_btn);
    lv_label_set_text(play_btn_label, LV_SYMBOL_PLAY " 播放");
    lv_obj_set_style_text_font(play_btn_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(play_btn_label, lv_color_white(), 0);
    lv_obj_center(play_btn_label);

    /* CS43131 电源测试按钮（关机/开机，供深度休眠低功耗验证） */
    const int dac_btn_w = 130, dac_btn_h = 36, dac_gap = 10;
    int dac_row_x = (EXAMPLE_LCD_H_RES - dac_btn_w * 2 - dac_gap) / 2;

    lv_obj_t *dac_off_btn = lv_btn_create(scr);
    lv_obj_set_size(dac_off_btn, dac_btn_w, dac_btn_h);
    lv_obj_set_pos(dac_off_btn, dac_row_x, 240);
    lv_obj_set_style_bg_color(dac_off_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_shadow_width(dac_off_btn, 4, 0);
    lv_obj_add_event_cb(dac_off_btn, dac_power_off_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dac_off_lbl = lv_label_create(dac_off_btn);
    lv_label_set_text(dac_off_lbl, LV_SYMBOL_POWER " DAC OFF");
    lv_obj_set_style_text_font(dac_off_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dac_off_lbl, lv_color_white(), 0);
    lv_obj_center(dac_off_lbl);

    lv_obj_t *dac_on_btn = lv_btn_create(scr);
    lv_obj_set_size(dac_on_btn, dac_btn_w, dac_btn_h);
    lv_obj_set_pos(dac_on_btn, dac_row_x + dac_btn_w + dac_gap, 240);
    lv_obj_set_style_bg_color(dac_on_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_shadow_width(dac_on_btn, 4, 0);
    lv_obj_add_event_cb(dac_on_btn, dac_power_on_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dac_on_lbl = lv_label_create(dac_on_btn);
    lv_label_set_text(dac_on_lbl, LV_SYMBOL_POWER " DAC ON");
    lv_obj_set_style_text_font(dac_on_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dac_on_lbl, lv_color_white(), 0);
    lv_obj_center(dac_on_lbl);

    /* DAC 状态标签（右侧，随电源操作实时刷新） */
    dac_status_label = lv_label_create(scr);
    lv_label_set_text(dac_status_label, "DAC: ON");
    lv_obj_set_style_text_font(dac_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dac_status_label, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_pos(dac_status_label, dac_row_x + dac_btn_w * 2 + dac_gap + 8, 247);

    /* 可拖动进度条（lv_slider，松手 seek） */
    progress_slider = lv_slider_create(scr);
    lv_obj_set_size(progress_slider, 380, 22);
    lv_obj_align(progress_slider, LV_ALIGN_TOP_MID, 0, 290);
    lv_slider_set_range(progress_slider, 0, 1);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_slider, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_add_event_cb(progress_slider, progress_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(progress_slider, progress_released_cb, LV_EVENT_RELEASED, NULL);

    /* 时间标签 */
    time_label = lv_label_create(scr);
    lv_label_set_text(time_label, "--:-- / --:--");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 324);

    /* 音量标签 */
    vol_label = lv_label_create(scr);
    lv_label_set_text_fmt(vol_label, LV_SYMBOL_VOLUME_MAX " %u%%", audio_get_volume());
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
    lv_obj_align(vol_label, LV_ALIGN_TOP_MID, 0, 372);

    /* 音量滑块 */
    lv_obj_t *vol_slider = lv_slider_create(scr);
    lv_obj_set_size(vol_slider, 340, 22);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_align(vol_slider, LV_ALIGN_TOP_MID, 0, 404);
    lv_obj_add_event_cb(vol_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 进度刷新定时器（500ms） */
    lv_timer_create(progress_timer_cb, 500, NULL);

    ESP_LOGI(TAG, "Main UI created (single track mp3 + DAC power test buttons)");
}

/* 当前播放曲目路径（main.c 开机播放共用） */
const char *main_ui_get_current_track(void)
{
    return TRACK_44100_PATH;
}
