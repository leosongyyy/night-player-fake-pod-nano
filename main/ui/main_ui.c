#include "ui/main_ui.h"

#include "core/fonts/system_smaller_font.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "main_ui";

void main_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* 占位主界面：仅一个居中的 Hello World 标签（播放列表/播放控制开发中） */
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello World");
    lv_obj_set_style_text_font(label, &system_smaller_font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    ESP_LOGI(TAG, "Main UI created (placeholder Hello World)");
}
