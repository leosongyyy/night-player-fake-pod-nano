#include "ui/settings_page.h"

#include "core/fonts/system_font.h"
#include "ui/list_common.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "settings_page";

lv_obj_t *settings_page_create(lv_obj_t *tv)
{
    lv_obj_t *page = lv_tabview_add_tab(tv, "");
    ui_page_style(page);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "播放设置");
    lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "此页面开发中");
    lv_obj_set_style_text_font(hint, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 30);

    ESP_LOGI(TAG, "settings page created (placeholder)");
    return page;
}
