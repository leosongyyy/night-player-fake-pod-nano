#include "ui/cassette/cassette_nav.h"

#include "core/utils/power_service.h"
#include "esp_log.h"
#include "ui/cassette/cassette_drawer.h"
#include "ui/cassette/cassette_fallback.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_ui.h"
#include "ui/cassette/m1_player.h"
#include "ui/cassette/m2_quick.h"
#include "ui/cassette/m3_weather.h"
#include "ui/cassette/m4_clock.h"
#include "ui/cassette/m5_list.h"
#include "ui/cassette/m6_settings.h"
#include "ui/cassette/m7_files.h"

static const char *TAG = "cas_nav";

/* 注册表：标题严格取 Web 各屏 .st 文案，短名取顶部机箱导航条按钮文案。
   T03 接入 M1/M2；T04/T05 接入 M3~M7，7 页 create/refresh 回调全部挂齐。 */
static cas_page_t s_pages[PAGE_COUNT] = {
    { PAGE_PLAYER,   "袖珍磁带机",     "磁带", NULL, false,
      cas_m1_create, cas_m1_refresh },
    { PAGE_QUICK,    "快捷控制中心",   "快捷", NULL, false,
      cas_m2_create, cas_m2_refresh },
    { PAGE_WEATHER,  "气象监测",       "天气", NULL, false,
      cas_m3_create, cas_m3_refresh },
    { PAGE_CLOCK,    "精密时钟",       "时钟", NULL, false,
      cas_m4_create, cas_m4_refresh },
    { PAGE_LIST,     "播放列表 · 甲面", "列表", NULL, false,
      cas_m5_create, cas_m5_refresh },
    { PAGE_SETTINGS, "系统设置",       "设置", NULL, false,
      cas_m6_create, cas_m6_refresh },
    { PAGE_FILES,    "文件管理",       "文件", NULL, false,
      cas_m7_create, cas_m7_refresh },
};

static cas_page_id_t s_active = PAGE_PLAYER;
static lv_obj_t *s_container = NULL;
static bool s_ready = false;

void cas_nav_init(lv_obj_t *pages_container)
{
    if (pages_container == NULL || s_ready) {
        return;
    }
    s_container = pages_container;
    s_ready = true;

    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *page = lv_obj_create(s_container);
        /* 页内容区避开顶部状态栏；透明背景让机箱壳透出。
           固定页清 SCROLLABLE，防止触摸误滚（F10）。 */
        lv_obj_set_size(page, EXAMPLE_LCD_H_RES,
                        EXAMPLE_LCD_V_RES - CAS_STATUS_H);
        lv_obj_set_pos(page, 0, CAS_STATUS_H);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

        s_pages[i].root = page;
        s_pages[i].created = true;
        /* T03~T05: 模块 create 回调将在此调用，填入 page root。 */
        if (s_pages[i].create != NULL) {
            s_pages[i].create(page);
        }
    }

    s_active = PAGE_PLAYER;
    ESP_LOGI(TAG, "7 pages registered (all resident, %d page slots)",
             (int)(sizeof(s_pages) / sizeof(s_pages[0])));
}

void cas_nav_show(cas_page_id_t id)
{
    if (!s_ready || id < 0 || id >= PAGE_COUNT) {
        return;
    }
    s_active = id;

    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (s_pages[i].root == NULL) {
            continue;
        }
        if (i == (int)id) {
            lv_obj_clear_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_pages[i].root);
        } else {
            lv_obj_add_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 状态栏标题 + 抽屉当前项高亮（两处同步，P0-2） */
    cas_shell_set_title(s_pages[id].title);
    cas_drawer_highlight(id);
    /* 切回时让页面按最新 cas_state_t 重刷一次：M5 甲乙面标题联动、M6 旋钮
       档位等都依赖此同步（create 期只刷一次，之后状态可能在别处被改）。 */
    if (s_pages[id].refresh != NULL) {
        s_pages[id].refresh();
    }
    ESP_LOGI(TAG, "show page %d: %s", (int)id, s_pages[id].title);
}

void cas_nav_next(void)
{
    cas_nav_show((cas_page_id_t)((s_active + 1) % PAGE_COUNT));
}

void cas_nav_prev(void)
{
    cas_nav_show((cas_page_id_t)((s_active + PAGE_COUNT - 1) % PAGE_COUNT));
}

bool cas_nav_on_key(button_event_t ev)
{
    /* 只在磁带 UI + 亮屏时接管短按；息屏与关机/息屏语义全部留给原逻辑。 */
    if (!cassette_ui_is_active() || !s_ready
            || power_service_is_screen_off()) {
        return false;
    }

    switch (ev) {
    case BUTTON_EVENT_UP_SHORT:
        cas_nav_next();
        return true;
    case BUTTON_EVENT_DOWN_SHORT:
        cas_nav_prev();
        return true;
    case BUTTON_EVENT_DOWN_LONG:
        cas_drawer_toggle();   /* 原 vol-10 改为抽屉开合（音量由抽屉推子承载） */
        return true;
    case BUTTON_EVENT_UP_LONG:
    case BUTTON_EVENT_BOTH:
    default:
        /* 安全语义不变：上长关机、双键息屏 */
        return false;
    }
}

cas_page_id_t cas_nav_active(void)
{
    return s_active;
}

const char *cas_nav_title(cas_page_id_t id)
{
    if (id < 0 || id >= PAGE_COUNT) {
        return "";
    }
    return s_pages[id].title;
}

const char *cas_nav_short_title(cas_page_id_t id)
{
    if (id < 0 || id >= PAGE_COUNT) {
        return "";
    }
    return s_pages[id].short_title;
}

lv_obj_t *cas_nav_page_root(cas_page_id_t id)
{
    if (id < 0 || id >= PAGE_COUNT) {
        return NULL;
    }
    return s_pages[id].root;
}
