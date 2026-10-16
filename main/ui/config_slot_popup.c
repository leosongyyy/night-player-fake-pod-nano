#include "ui/config_slot_popup.h"

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/storage/config_slots.h"
#include "core/storage/config_store.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "slot_popup";

/* ------------------------------------------------------------------ */
/* 面板布局参数与配色（与 ui/eq_popup.c 保持一致，弹窗观感统一）          */
/* ------------------------------------------------------------------ */

#define SLOT_POPUP_SCREEN_W    EXAMPLE_LCD_H_RES   /* 460 */
#define SLOT_POPUP_SCREEN_H    EXAMPLE_LCD_V_RES   /* 460 */

/* 面板宽 380：左右各留 40 边缘（点击边缘 = 关闭弹窗的手势区） */
#define SLOT_POPUP_W           380
#define SLOT_POPUP_ROW_H       52      /* 每行高 */
#define SLOT_POPUP_ROW_GAP     8       /* 行间距（列表 flex pad_row） */
#define SLOT_POPUP_PAD         14      /* 面板内边距 */
#define SLOT_POPUP_TITLE_GAP   12      /* 标题与列表间距 */
#define SLOT_POPUP_MAX_VISIBLE 6       /* 列表最多同时可见行数（超出滚动） */

#define SLOT_POPUP_BG          lv_color_hex(0x141414)   /* 面板底 */
#define SLOT_POPUP_ROW_BG      lv_color_hex(0x1E1E1E)   /* 行卡片：深灰 */
#define SLOT_POPUP_PRESS       lv_color_hex(0xFFC000)   /* 按下高亮：黄 */
#define SLOT_POPUP_TEXT        lv_color_white()
#define SLOT_POPUP_HL          lv_palette_main(LV_PALETTE_YELLOW)  /* 当前活跃槽位 */

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_root   = NULL;  /* 全屏遮罩（点击空白关闭） */
static lv_obj_t *s_panel  = NULL;  /* 中央面板 */
static lv_obj_t *s_list   = NULL;  /* 选项行容器（可滚动） */
static lv_obj_t *s_anchor = NULL;  /* 挂载页面（播放设置页 tab） */
static bool      s_visible = false;

/* ------------------------------------------------------------------ */
/* 行交互                                                             */
/* ------------------------------------------------------------------ */

/* 行点击：idx = 槽位下标。立即切换配置并应用（config_slots_switch 内部会
   保存旧槽位、载入新槽位、应用音量/EQ/睡眠定时/播放器，并弹 Toast）。 */
static void slot_popup_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= config_store_slot_count()) {
        return;
    }
    config_slots_switch(idx);   /* 同槽位/越界在内部直接返回 */
    config_slot_popup_toggle(s_anchor);   /* 关闭弹窗 */
}

/* 遮罩点击：仅当事件目标就是遮罩本身（点击行/面板不冒泡误关，
   LVGL 事件 target 保持最初对象）时关闭弹窗 */
static void slot_popup_bg_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (lv_event_get_target(e) == s_root) {
        config_slot_popup_toggle(s_anchor);
    }
}

/* ------------------------------------------------------------------ */
/* 列表重建（每次打开时重新读槽位摘要）                                 */
/* ------------------------------------------------------------------ */

/* 创建一行：text 显示文字，hl = 当前活跃槽位高亮，idx 作为事件 user_data */
static void slot_popup_row_add(lv_obj_t *list, const char *text, bool hl,
                               uint32_t idx)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, SLOT_POPUP_W - SLOT_POPUP_PAD * 2, SLOT_POPUP_ROW_H);

    lv_obj_set_style_bg_color(row, SLOT_POPUP_ROW_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    /* 按下时黄色背景 */
    lv_obj_set_style_bg_color(row, SLOT_POPUP_PRESS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    /* 单行水平排列，文字垂直居中 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, hl ? SLOT_POPUP_HL : SLOT_POPUP_TEXT, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);   /* 超长裁切 */
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_add_event_cb(row, slot_popup_row_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
}

/* 重建列表内容并按内容调整面板尺寸（每次打开弹窗前调用）
   行文本：SAVE<n> <该配置里的播放列表名>；槽位无列表名（空槽位/只存过
   播放位置而无歌曲）时只显示 SAVE<n>。 */
static void slot_popup_rebuild(void)
{
    uint32_t count = config_store_slot_count();
    uint32_t active = config_store_get_active_slot();

    lv_obj_clean(s_list);

    for (uint32_t i = 0; i < count; i++) {
        config_store_slot_info_t info;
        bool has = config_store_get_slot_info(i, &info);

        char text[CONFIG_STORE_SLOT_NAME_MAX + 16];
        if (has && info.list_name[0] != '\0') {
            snprintf(text, sizeof(text), "SAVE%u %s", (unsigned)(i + 1u),
                     info.list_name);
        } else {
            snprintf(text, sizeof(text), "SAVE%u", (unsigned)(i + 1u));
        }
        slot_popup_row_add(s_list, text, i == active, i);
    }

    /* 列表可视高度：最多 SLOT_POPUP_MAX_VISIBLE 行（超出内部滚动） */
    int rows = (int)count;
    if (rows < 1) {
        rows = 1;
    }
    int visible = rows > SLOT_POPUP_MAX_VISIBLE ? SLOT_POPUP_MAX_VISIBLE : rows;
    int list_h = visible * SLOT_POPUP_ROW_H + (visible - 1) * SLOT_POPUP_ROW_GAP;
    lv_obj_set_size(s_list, SLOT_POPUP_W - SLOT_POPUP_PAD * 2, list_h);

    /* 面板总高 = 上下 padding + 标题 + 间距 + 列表 */
    int panel_h = SLOT_POPUP_PAD * 2 + 24 + SLOT_POPUP_TITLE_GAP + list_h;
    lv_obj_set_size(s_panel, SLOT_POPUP_W, panel_h);
    lv_obj_set_pos(s_panel, (SLOT_POPUP_SCREEN_W - SLOT_POPUP_W) / 2,
                   (SLOT_POPUP_SCREEN_H - panel_h) / 2 - 8);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void config_slot_popup_toggle(lv_obj_t *anchor_page)
{
    /* 首次调用：懒创建弹窗对象（挂在 anchor 页上） */
    if (s_root == NULL) {
        if (anchor_page == NULL) {
            return;
        }
        s_anchor = anchor_page;

        /* 全屏遮罩：半透明黑，点击空白关闭 */
        s_root = lv_obj_create(s_anchor);
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_size(s_root, SLOT_POPUP_SCREEN_W, SLOT_POPUP_SCREEN_H);
        lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_50, 0);
        lv_obj_set_style_border_width(s_root, 0, 0);
        lv_obj_set_style_pad_all(s_root, 0, 0);
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(s_root, slot_popup_bg_cb, LV_EVENT_CLICKED, NULL);

        /* 中央面板：纵向排列（标题 + 列表），尺寸在 rebuild 时按内容计算 */
        s_panel = lv_obj_create(s_root);
        lv_obj_set_style_bg_color(s_panel, SLOT_POPUP_BG, 0);
        lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_panel, 14, 0);
        lv_obj_set_style_border_width(s_panel, 0, 0);
        lv_obj_set_style_pad_all(s_panel, SLOT_POPUP_PAD, 0);
        lv_obj_set_style_pad_top(s_panel, 16, 0);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *title = lv_label_create(s_panel);
        lv_label_set_text(title, "配置切换");
        lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(title, SLOT_POPUP_TEXT, 0);

        /* 列表容器：flex 纵向，超出行数内部滚动 */
        s_list = lv_obj_create(s_panel);
        lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_list, 0, 0);
        lv_obj_set_style_pad_all(s_list, 0, 0);
        lv_obj_set_style_pad_row(s_list, SLOT_POPUP_ROW_GAP, 0);
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);  /* 先清，下面再加 */
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);   /* 初始隐藏 */
    }

    /* 切换显示状态 */
    if (s_visible) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        s_visible = false;
        ESP_LOGI(TAG, "popup hidden");
        return;
    }

    /* 打开：重新读槽位摘要并重建列表（列表名/活跃槽位可能已变） */
    slot_popup_rebuild();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    ESP_LOGI(TAG, "popup shown (active slot %u)",
             (unsigned)(config_store_get_active_slot() + 1u));
}

bool config_slot_popup_is_visible(void)
{
    return s_visible;
}
