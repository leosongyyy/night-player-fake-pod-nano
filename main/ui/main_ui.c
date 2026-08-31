#include "ui/main_ui.h"

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "ui/app.h"
#include "ui/dir_list.h"
#include "ui/list_common.h"
#include "ui/now_playing.h"
#include "ui/playlist_page.h"
#include "ui/settings_page.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "main_ui";

/* tabview 句柄（供 main_ui_switch_tab 程序化翻页） */
static lv_obj_t *s_tv = NULL;

/* 同步当前页状态：列表页（0/1）允许跑马灯，非列表页（2/3）关闭省电；
   同时把状态栏中央的页面名称更新为当前页。
   注意：lv_tabview_set_act 编程切换时不会触发 VALUE_CHANGED 事件
   （cont_scroll_end_event_cb 中 new_tab=false 不发事件），因此编程切换
   后需手动调用本函数；手势滑动则由 main_tabs_changed_cb 调用。 */
static void main_tabs_sync(void)
{
    uint16_t act = lv_tabview_get_tab_act(s_tv);
    bool is_list_page = (act == 0 || act == 1);

    dir_list_set_marquee(is_list_page);
    playlist_page_set_marquee(is_list_page);
    /* 正在播放页的标题/副标题跑马灯：仅当该页为当前页时滚动 */
    now_playing_set_marquee(act == 2);
    app_status_set_page(act);

    /* 离开正在播放页时，强制恢复播放页的标题栏/控制栏与顶部状态栏显示：
       用户在播放页点封面隐藏的栏（含联动的状态栏）在下次进入前复位，
       避免其它页面缺少状态栏 */
    if (act != 2) {
        now_playing_force_show_ui();
        app_status_set_ui_visible(true);
    }

    ESP_LOGI(TAG, "tab active %u, marquee %s",
             (unsigned)act, is_list_page ? "on" : "off");
}

/* 程序化切换到指定页面（无动画直接切换，等同 ViewPager 瞬移）。
   手指滑动翻页的惯性/回弹动画不受影响（由 LVGL 手势控制）。 */
void main_ui_switch_tab(uint8_t tab)
{
    if (s_tv == NULL) {
        return;
    }
    lv_tabview_set_act(s_tv, tab, LV_ANIM_OFF);
    main_tabs_sync();   /* set_act 不发 VALUE_CHANGED，需手动同步状态栏/跑马灯 */
}

/* 获取当前页面下标（供状态栏等晚于主界面创建的部分初始化时同步） */
uint8_t main_ui_get_current_tab(void)
{
    if (s_tv == NULL) {
        return 0;
    }
    return (uint8_t)lv_tabview_get_tab_act(s_tv);
}

/* tabview 切换页后回调（手势滑动触发）：同步状态栏页面名与跑马灯 */
static void main_tabs_changed_cb(lv_event_t *e)
{
    (void)e;
    main_tabs_sync();
}

void main_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* 四页滑动主界面：基于 LVGL 内置 lv_tabview（demo_widgets 同款设计），
       隐藏顶部 tab 按钮条后即为纯横向滑动翻页（类 Android ViewPager）：
       0=目录列表 / 1=播放列表 / 2=正在播放(占位) / 3=播放设置(占位)。
       容器撑满全屏（460x460），页面内容由各页内部偏移避开顶部状态栏区，
       为后续沉浸式封面（内容延伸到状态栏下方）做准备。 */
    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 0);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_size(tv, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    s_tv = tv;

    /* 隐藏 tab 按钮条（保留滑动切换），内容区占满。
       LVGL 8 无 lv_obj_set_style_display，用 HIDDEN 标志（flex 布局中不占空间） */
    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    lv_obj_add_flag(btns, LV_OBJ_FLAG_HIDDEN);

    /* tabview 内容容器：黑底、无边框、无内边距 */
    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);

    /* 四个页面 */
    dir_list_create(tv);
    playlist_page_create(tv);
    now_playing_create(tv);
    settings_page_create(tv);

    /* 页面切换时控制列表跑马灯（非列表页关闭省电） */
    lv_obj_add_event_cb(tv, main_tabs_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 启动默认停在正在播放页（tab 2）。
       此前默认该页时首帧渲染全屏封面耗时较长，全程亮度 0 再点亮偶发黑屏；
       已配合 display_init 的 2% 微亮初始化规避（见 display.h） */
    main_ui_switch_tab(2);

    ESP_LOGI(TAG, "Main UI created (4-page swipe pager)");
}
