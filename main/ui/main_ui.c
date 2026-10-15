#include "ui/main_ui.h"

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "core/ui/click_wheel.h"
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
    /* 虚拟 ClickWheel（顶层悬浮按钮）：仅目录页/歌曲页显示，
       其滚动目标由 click_wheel_set_list 在列表构建时登记 */
    click_wheel_set_tab(act);
    /* 正在播放页的标题/副标题跑马灯：仅当该页为当前页时滚动 */
    now_playing_set_marquee(act == 2);
    app_status_set_page(act);

    /* 离开正在播放页时，强制恢复播放页的标题栏/控制栏与顶部状态栏显示：
       用户在播放页点封面隐藏的栏（含联动的状态栏）在下次进入前复位，
       避免其它页面缺少状态栏。
       状态栏背景强制保持不透明纯黑：半透明仅用于播放页——松手时
       VALUE_CHANGED 会在惯性滚动动画结束前触发本函数，若不覆盖，
       app_status_set_ui_visible 会把状态栏立即改回半透明，导致
       惯性滚动期间状态栏提前变透明。 */
    if (act != 2) {
        now_playing_force_show_ui();
        app_status_set_ui_visible(true);
        app_status_set_bg_opa(LV_OPA_COVER, false);
    }

    ESP_LOGI(TAG, "tab active %u, marquee %s",
             (unsigned)act, is_list_page ? "on" : "off");
}

/* 程序化切页核心：anim 决定是否带横向滚动动画。tab_cur 在 set_act 内立即
   更新为目标页（动画期间即已切换），lv_tabview_set_act 编程切换不触发
   VALUE_CHANGED（见 main_tabs_sync 注释），因此两种模式都需手动同步
   状态栏/跑马灯。 */
static void main_ui_switch_tab_inner(uint8_t tab, bool anim)
{
    if (s_tv == NULL) {
        return;
    }
    lv_tabview_set_act(s_tv, tab, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    main_tabs_sync();   /* set_act 不发 VALUE_CHANGED，需手动同步状态栏/跑马灯 */
}

/* 程序化切换到指定页面（无动画直接切换，等同 ViewPager 瞬移）。
   手指滑动翻页的惯性/回弹动画不受影响（由 LVGL 手势控制）。 */
void main_ui_switch_tab(uint8_t tab)
{
    main_ui_switch_tab_inner(tab, false);
}

/* 程序化切换到指定页面（带动画，等同 ViewPager 翻页）。
   用户可感知的翻页操作（点击当前歌曲跳正在播放页、点击目录进歌曲列表）使用。 */
void main_ui_switch_tab_anim(uint8_t tab)
{
    main_ui_switch_tab_inner(tab, true);
}

/* 获取当前页面下标（供状态栏等晚于主界面创建的部分初始化时同步） */
uint8_t main_ui_get_current_tab(void)
{
    if (s_tv == NULL) {
        return 0;
    }
    return (uint8_t)lv_tabview_get_tab_act(s_tv);
}

/* 显示/隐藏整个主界面（供全屏覆盖层如设置菜单使用）。
   仅切换 HIDDEN 标志，页面状态全部保留；隐藏期间底层页面（尤其播放页
   460x460 封面大图）不参与重绘，消除菜单滚动时被底层大图拖慢的卡顿。 */
void main_ui_set_visible(bool visible)
{
    if (s_tv == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_tv, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tv, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "main UI %s", visible ? "shown" : "hidden");
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
       0=目录/歌单列表 / 1=歌曲列表 / 2=正在播放 / 3=播放设置。
       容器撑满全屏（460x460），页面内容由各页内部偏移避开顶部状态栏区，
       为后续沉浸式封面（内容延伸到状态栏下方）做准备。 */
    /* v9 的 lv_tabview_create() 不再接收方向与尺寸参数，改为创建后单独设置 */
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, 0);
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

    /* 虚拟 ClickWheel：创建在 lv_scr_act()（屏幕层，tabview 的兄弟），
       整个应用只需一个实例。
       ⚠ 放屏幕层（tabview 之外）而不是 tab 页容器内：放在页面容器内时，
       从按钮起手拖动会被 LVGL 的滚动链（SCROLL_CHAIN）找到可横向滚动的
       tabview → 判定为翻页并给按钮发 PRESS_LOST，"按住画圈"直接失效
       （见 core/ui/click_wheel.h）。
       ⚠ 也不要挂 lv_layer_top()：会浮在下拉控制中心之上，而"把面板也挂
       顶层"的修法会破坏 LVGL 的覆盖判定 → 下拉面板时被遮挡的页面内容
       每帧重绘、严重掉帧（见 DONE_PLANS.md 第 38 点）。
       显隐由 main_tabs_sync（当前页面）与各列表构建时登记的目标共同决定；
       功能开关/方向/速度档位来自 NVS 配置（click_wheel / click_wheel_rev /
       click_wheel_spd，设置菜单 → 虚拟 ClickWheel 选项 子页可调，立即生效） */
    click_wheel_create();
    click_wheel_set_enabled(config_store_get_click_wheel());
    /* 方向（正常/反转）与速度档位（普通/快速/极速）同样来自 NVS
       （设置菜单 → 虚拟 ClickWheel 选项 子页），开机按配置初始化 */
    click_wheel_set_reverse(config_store_get_click_wheel_reverse());
    click_wheel_set_speed(config_store_get_click_wheel_speed());

    /* 页面切换时控制列表跑马灯（非列表页关闭省电） */
    lv_obj_add_event_cb(tv, main_tabs_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 启动默认停在正在播放页（tab 2）。
       此前默认该页时首帧渲染全屏封面耗时较长，全程亮度 0 再点亮偶发黑屏；
       已配合 display_init 的 2% 微亮初始化规避（见 display.h） */
    main_ui_switch_tab(2);

    ESP_LOGI(TAG, "Main UI created (4-page swipe pager)");
}
