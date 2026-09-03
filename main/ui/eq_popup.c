#include "ui/eq_popup.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/eq.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "ui/toast.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "eq_popup";

/* ------------------------------------------------------------------ */
/* 面板布局参数与配色（暗色主题，与 settings_menu/settings_page 一致）   */
/* ------------------------------------------------------------------ */

#define EQ_POPUP_SCREEN_W    EXAMPLE_LCD_H_RES   /* 460 */
#define EQ_POPUP_SCREEN_H    EXAMPLE_LCD_V_RES   /* 460 */

/* 面板宽 380：左右各留 40 边缘（点击边缘 = 关闭弹窗的手势区） */
#define EQ_POPUP_W           380
#define EQ_POPUP_ROW_H       52      /* 每行高 */
#define EQ_POPUP_ROW_GAP     8       /* 行间距（列表 flex pad_row） */
#define EQ_POPUP_PAD         14      /* 面板内边距 */
#define EQ_POPUP_TITLE_GAP   12      /* 标题与列表间距 */
#define EQ_POPUP_MAX_VISIBLE 5       /* 列表最多同时可见行数（超出滚动） */
#define EQ_POPUP_MAX_FILES   24      /* 最多列出的 .eq 文件数 */

#define EQ_POPUP_BG          lv_color_hex(0x141414)   /* 面板底（比行卡片深） */
#define EQ_POPUP_ROW_BG      lv_color_hex(0x1E1E1E)   /* 行卡片：深灰 */
#define EQ_POPUP_PRESS       lv_color_hex(0xFFC000)   /* 按下高亮：黄 */
#define EQ_POPUP_TEXT        lv_color_white()
#define EQ_POPUP_DIM         lv_palette_main(LV_PALETTE_GREY)
#define EQ_POPUP_HL          lv_palette_main(LV_PALETTE_YELLOW)  /* 当前生效项 */

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_root   = NULL;  /* 全屏遮罩（点击空白关闭） */
static lv_obj_t *s_panel  = NULL;  /* 中央面板 */
static lv_obj_t *s_list   = NULL;  /* 选项行容器（可滚动） */
static lv_obj_t *s_anchor = NULL;  /* 挂载页面（settings_page 的 tab 页） */

/* 本次打开时扫描到的 .eq 文件名（弹窗存活期间有效；行 user_data 指向这里） */
static char s_files[EQ_POPUP_MAX_FILES][EQ_FILE_NAME_MAX];
static int  s_file_count = 0;
static bool s_visible = false;

/* ------------------------------------------------------------------ */
/* 行交互                                                             */
/* ------------------------------------------------------------------ */

/* 行点击：idx 0 = 无 EQ；1..n = s_files[idx-1] */
static void eq_popup_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);

    /* 关闭 EQ */
    if (idx == 0) {
        eq_apply(NULL);
        config_store_set_eq(NULL, NULL);   /* 持久化清除（通知设置页刷新） */
        ui_toast_show("EQ 已关闭");
        eq_popup_toggle(s_anchor);   /* 关闭弹窗 */
        return;
    }

    /* 解析选中的 .eq 文件（同步读 SD 卡，文件仅 KB 级，毫秒内完成） */
    if (idx - 1 >= s_file_count) {
        return;
    }
    const char *name = s_files[idx - 1];
    eq_preset_t preset;
    int rc = eq_parse_named(name, &preset);
    if (rc == EQ_PARSE_OK) {
        eq_apply(&preset);
        config_store_set_eq(name, &preset);   /* 参数入 NVS（通知设置页刷新） */
        ui_toast_show("已应用 EQ");
        ESP_LOGI(TAG, "EQ preset applied: %s (%u bands)", name,
                 (unsigned)preset.band_count);
        eq_popup_toggle(s_anchor);   /* 关闭弹窗 */
    } else {
        /* 解析失败：Toast 说明原因（FUTURE_PLAN 第 10 点），弹窗保持打开
           供用户改选其它预设 */
        const char *msg = "EQ 文件内容无效";
        switch (rc) {
        case EQ_PARSE_ERR_TOO_MANY: msg = "EQ 段数超过 10，无法使用"; break;
        case EQ_PARSE_ERR_INVALID:  msg = "EQ 文件内容无效";          break;
        case EQ_PARSE_ERR_EMPTY:    msg = "EQ 文件为空";              break;
        case EQ_PARSE_ERR_OPEN:     msg = "无法读取 EQ 文件";         break;
        default: break;
        }
        ESP_LOGW(TAG, "parse %s failed: rc=%d", name, rc);
        ui_toast_show(msg);
    }
}

/* 遮罩点击：仅当事件目标就是遮罩本身（点击行/面板不冒泡误关，
   LVGL 事件 target 保持最初对象）时关闭弹窗 */
static void eq_popup_bg_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (lv_event_get_target(e) == s_root) {
        eq_popup_toggle(s_anchor);
    }
}

/* ------------------------------------------------------------------ */
/* 列表重建（每次打开时重新扫描 SD 卡）                                 */
/* ------------------------------------------------------------------ */

/* 创建一行：text 显示文字，hl = 当前生效高亮，clickable = 是否可点击，
   idx 作为事件 user_data（-1 = 不可点提示行） */
static void eq_popup_row_add(lv_obj_t *list, const char *text, bool hl,
                             bool clickable, int idx)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, EQ_POPUP_W - EQ_POPUP_PAD * 2, EQ_POPUP_ROW_H);

    lv_obj_set_style_bg_color(row, EQ_POPUP_ROW_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    /* 按下时黄色背景（不可点行不设置） */
    if (clickable) {
        lv_obj_set_style_bg_color(row, EQ_POPUP_PRESS, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
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
    lv_obj_set_style_text_color(lbl, hl ? EQ_POPUP_HL : EQ_POPUP_TEXT, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);   /* 超长裁切 */
    lv_obj_set_flex_grow(lbl, 1);

    if (clickable) {
        lv_obj_add_event_cb(row, eq_popup_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)idx);
    }
}

/* 重建列表内容并按内容调整面板尺寸（每次打开弹窗前调用） */
static void eq_popup_rebuild(void)
{
    /* 重新扫描 .musicfx 目录下的 .eq 文件 */
    s_file_count = eq_scan_files(s_files, EQ_POPUP_MAX_FILES);
    if (s_file_count >= EQ_POPUP_MAX_FILES) {
        ESP_LOGW(TAG, "too many eq files, list truncated to %d",
                 (int)EQ_POPUP_MAX_FILES);
    }

    /* 当前生效文件名（高亮用） */
    char cur_name[EQ_FILE_NAME_MAX] = "";
    config_store_get_eq(cur_name, sizeof(cur_name), NULL);

    /* 清空旧行 */
    lv_obj_clean(s_list);

    /* 行数 = 无 EQ + 文件数 + （无文件时的提示行） */
    int rows = 1 + s_file_count;
    if (s_file_count == 0) {
        rows += 1;   /* "未找到 EQ 文件"提示行 */
    }

    /* 第一行：无 EQ（关闭）——当前无 EQ 时高亮 */
    eq_popup_row_add(s_list, "无 EQ（关闭）", cur_name[0] == '\0', true, 0);

    /* 各 .eq 文件行 */
    for (int i = 0; i < s_file_count; i++) {
        eq_popup_row_add(s_list, s_files[i],
                         strcmp(s_files[i], cur_name) == 0, true, i + 1);
    }
    /* 无文件提示（不可点） */
    if (s_file_count == 0) {
        eq_popup_row_add(s_list, "未在 .musicfx 找到 EQ 文件", false, false, -1);
    }

    /* 列表可视高度：最多 EQ_POPUP_MAX_VISIBLE 行（超出内部滚动） */
    int visible = rows > EQ_POPUP_MAX_VISIBLE ? EQ_POPUP_MAX_VISIBLE : rows;
    int list_h = visible * EQ_POPUP_ROW_H + (visible - 1) * EQ_POPUP_ROW_GAP;
    lv_obj_set_size(s_list, EQ_POPUP_W - EQ_POPUP_PAD * 2, list_h);

    /* 面板总高 = 上下 padding + 标题 + 间距 + 列表 */
    int panel_h = EQ_POPUP_PAD * 2 + 24 + EQ_POPUP_TITLE_GAP + list_h;
    lv_obj_set_size(s_panel, EQ_POPUP_W, panel_h);
    lv_obj_set_pos(s_panel, (EQ_POPUP_SCREEN_W - EQ_POPUP_W) / 2,
                   (EQ_POPUP_SCREEN_H - panel_h) / 2 - 8);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void eq_popup_toggle(lv_obj_t *anchor_page)
{
    /* 首次调用：懒创建弹窗对象（挂在 anchor 页上） */
    if (s_root == NULL) {
        if (anchor_page == NULL) {
            return;
        }
        s_anchor = anchor_page;

        /* 全屏遮罩：半透明黑，点击空白关闭（点击面板区域不冒泡误关，
           见 eq_popup_bg_cb 的 target 判断） */
        s_root = lv_obj_create(s_anchor);
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_size(s_root, EQ_POPUP_SCREEN_W, EQ_POPUP_SCREEN_H);
        lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_50, 0);
        lv_obj_set_style_border_width(s_root, 0, 0);
        lv_obj_set_style_pad_all(s_root, 0, 0);
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(s_root, eq_popup_bg_cb, LV_EVENT_CLICKED, NULL);

        /* 中央面板：纵向排列（标题 + 列表），尺寸在 rebuild 时按内容计算 */
        s_panel = lv_obj_create(s_root);
        lv_obj_set_style_bg_color(s_panel, EQ_POPUP_BG, 0);
        lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_panel, 14, 0);
        lv_obj_set_style_border_width(s_panel, 0, 0);
        lv_obj_set_style_pad_all(s_panel, EQ_POPUP_PAD, 0);
        lv_obj_set_style_pad_top(s_panel, 16, 0);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *title = lv_label_create(s_panel);
        lv_label_set_text(title, "选择 EQ");
        lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(title, EQ_POPUP_TEXT, 0);

        /* 列表容器：flex 纵向，超出行数内部滚动 */
        s_list = lv_obj_create(s_panel);
        lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_list, 0, 0);
        lv_obj_set_style_pad_all(s_list, 0, 0);
        lv_obj_set_style_pad_row(s_list, EQ_POPUP_ROW_GAP, 0);
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

    /* 打开：重新扫描并重建列表（用户可能新拷贝了 .eq 文件） */
    eq_popup_rebuild();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    ESP_LOGI(TAG, "popup shown (%d eq files)", s_file_count);
}

bool eq_popup_is_visible(void)
{
    return s_visible;
}
