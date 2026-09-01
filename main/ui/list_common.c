#include "ui/list_common.h"

#include <string.h>

#include "core/fonts/system_font.h"

/* 标题栏内用户数据：保存标题/计数 label 句柄，随标题栏对象一起管理 */
typedef struct {
    lv_obj_t *title_lbl;
    lv_obj_t *sub_lbl;
} ui_title_bar_t;

/* 标题栏被删除时释放用户数据结构（防止 lv_obj 删除后 user_data 泄漏） */
static void title_bar_delete_cb(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    lv_mem_free(lv_obj_get_user_data(bar));
}

void ui_page_style(lv_obj_t *page)
{
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t *ui_title_bar_create(lv_obj_t *parent, const char *title,
                              const char *sub)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), UI_TITLE_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    /* 底部分隔线：与内容区之间 1px 灰线 */
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    ui_title_bar_t *t = lv_mem_alloc(sizeof(*t));
    if (t == NULL) {
        lv_obj_del(bar);
        return NULL;
    }
    memset(t, 0, sizeof(*t));

    t->title_lbl = lv_label_create(bar);
    lv_label_set_text(t->title_lbl, title);
    lv_obj_set_style_text_font(t->title_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(t->title_lbl, lv_color_white(), 0);
    /* 标题占标题栏左侧 3/4 宽（与右侧计数 1/4 按 3:1 划分，互不覆盖）：
       超长文本直接裁剪（不换行、不跑马灯），保证永远不盖住计数显示 */
    lv_obj_set_width(t->title_lbl, LV_PCT(75));
    lv_obj_set_style_pad_left(t->title_lbl, 16, 0);
    lv_label_set_long_mode(t->title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(t->title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    if (sub != NULL) {
        t->sub_lbl = lv_label_create(bar);
        lv_label_set_text(t->sub_lbl, sub);
        lv_obj_set_style_text_font(t->sub_lbl, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(t->sub_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
        /* 计数占标题栏右侧 1/4 宽：文本右对齐，超长同样裁剪 */
        lv_obj_set_width(t->sub_lbl, LV_PCT(25));
        lv_obj_set_style_pad_right(t->sub_lbl, 16, 0);
        lv_obj_set_style_text_align(t->sub_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(t->sub_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(t->sub_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_set_user_data(bar, t);
    lv_obj_add_event_cb(bar, title_bar_delete_cb, LV_EVENT_DELETE, NULL);
    return bar;
}

void ui_title_bar_set_text(lv_obj_t *bar, const char *title, const char *sub)
{
    if (bar == NULL) {
        return;
    }
    ui_title_bar_t *t = (ui_title_bar_t *)lv_obj_get_user_data(bar);
    if (t == NULL) {
        return;
    }

    if (title != NULL && t->title_lbl != NULL) {
        lv_label_set_text(t->title_lbl, title);
    }

    if (sub != NULL) {
        if (t->sub_lbl == NULL) {
            /* 首次出现计数文字时动态创建（占右侧 1/4 宽、右对齐灰色小字） */
            t->sub_lbl = lv_label_create(bar);
            lv_obj_set_style_text_font(t->sub_lbl, UI_SYSTEM_FONT, 0);
            lv_obj_set_style_text_color(t->sub_lbl,
                                        lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_width(t->sub_lbl, LV_PCT(25));
            lv_obj_set_style_pad_right(t->sub_lbl, 16, 0);
            lv_obj_set_style_text_align(t->sub_lbl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(t->sub_lbl, LV_LABEL_LONG_CLIP);
            lv_obj_align(t->sub_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_label_set_text(t->sub_lbl, sub);
        lv_obj_clear_flag(t->sub_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (t->sub_lbl != NULL) {
        /* sub 为 NULL：隐藏右侧文字 */
        lv_obj_add_flag(t->sub_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}
