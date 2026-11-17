#include "ui/cassette/m7_files.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m7";

/* 静态目录树（Web FS 的 pinned 部分，全部「（示例）」） */
typedef enum { FT_DIR = 0, FT_AUD } cas_ftype_t;
typedef struct {
    cas_ftype_t type;
    const char *name;
    const char *meta;   /* 目录="N项" / 音频="42.5MB" 或时长 */
} cas_fentry_t;

/* 当前只展示"磁带音乐库"层级的固定 5 行（架构 P0：静态树，不读真实 FS） */
static const cas_fentry_t s_entries[5] = {
    { FT_DIR, "甲面·复古流行精选", "12项" },
    { FT_DIR, "乙面·深夜爵士慢摇", "8项" },
    { FT_DIR, "磁带现场录音归档",   "5项" },
    { FT_AUD, "落日磁带机_母带.wav", "42.5MB" },
    { FT_AUD, "暖风与旧广播.flac",   "28.1MB" },
};

#define M7_PAD       CAS_CQW(3)
#define M7_W         (EXAMPLE_LCD_H_RES - 2 * M7_PAD)
#define M7_CAP_Y     CAS_CQW(1.5)
#define M7_CAP_H     CAS_CQW(8)
#define M7_PATH_Y    (M7_CAP_Y + M7_CAP_H + CAS_CQW(1.2))
#define M7_PATH_H    CAS_CQW(8)
#define M7_ROWS_Y    (M7_PATH_Y + M7_PATH_H + CAS_CQW(1.2))
#define M7_ROW_H     CAS_CQW(10)
#define M7_ROW_GAP   CAS_CQW(0.6)
#define M7_DETAIL_Y  (M7_ROWS_Y + 5 * (M7_ROW_H + M7_ROW_GAP) + CAS_CQW(0.5))
#define M7_DETAIL_H  CAS_CQW(9)
#define M7_KEYS_Y    (M7_DETAIL_Y + M7_DETAIL_H + CAS_CQW(1))
#define M7_KEYS_H    CAS_CQW(10)

typedef enum { M7_OPEN = 0, M7_NEW, M7_DETAIL, M7_KEY_COUNT } m7_key_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_capbar = NULL;
static lv_obj_t *s_path = NULL;
static lv_obj_t *s_rows[5];
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_detail_label = NULL;
static lv_obj_t *s_keys[M7_KEY_COUNT];

/* ---------------------- 事件 ---------------------- */

static void m7_row_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 5u) {
        return;
    }
    cas_state_t *st = cas_state_get();
    st->sel = (int8_t)idx;
    cas_m7_refresh();
}

static void m7_back_cb(lv_event_t *e)
{
    (void)e;
    cas_state_t *st = cas_state_get();
    if (st->path_len > 1) {
        st->path_len--;
        st->sel = -1;
        st->detail = false;
    }
    /* P1: 真实目录 up（FS 浏览属 Q5 待确认） */
    cas_m7_refresh();
}

static void m7_key_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(e);
    if (raw >= (uintptr_t)M7_KEY_COUNT) {
        return;
    }
    cas_state_t *st = cas_state_get();
    switch ((m7_key_t)raw) {
    case M7_OPEN:
        /* P1: player_controller_play_file(s_entries[st->sel].name) */
        break;
    case M7_NEW:
        /* 新建目录（示例）：仅切 UI 提示，不落盘（Web 同款） */
        if (st->path_len < CAS_PATH_DEPTH) {
            strncpy(st->path[st->path_len], "新建目录（示例）", CAS_PATH_MAX - 1);
            st->path_len++;
        }
        break;
    case M7_DETAIL:
        st->detail = !st->detail;
        break;
    default:
        break;
    }
    cas_m7_refresh();
}

void cas_m7_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;

    /* 容量条 + 文案（18.6GB/32GB，已用 58.1%，三色分段） */
    s_capbar = cas_capbar_create(s_root, M7_W, M7_CAP_H);
    lv_obj_set_pos(s_capbar, M7_PAD, M7_CAP_Y);
    cas_capbar_set_pct(s_capbar, 58);
    lv_obj_t *caplab = lv_label_create(s_root);
    lv_label_set_text(caplab, "内置存储：已用 18.6GB / 32GB（示例）");
    /* OBS-06：数字/容量文本统一走等宽数字字体，避免刷新时横向抖动 */
    lv_obj_set_style_text_font(caplab, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(caplab, CAS_COL_FG, 0);
    lv_obj_set_pos(caplab, M7_PAD, M7_CAP_Y + M7_CAP_H + 1);

    /* 面包屑行：返回上级 + 路径（等高宽按钮） */
    lv_obj_t *back = lv_obj_create(s_root);
    lv_obj_set_size(back, CAS_CQW(20), M7_PATH_H);
    lv_obj_set_pos(back, M7_PAD, M7_PATH_Y);
    lv_obj_add_style(back, cas_style_metal(), 0);
    lv_obj_set_style_radius(back, 5, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回上级");
    lv_obj_set_style_text_font(back_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(back_lbl, CAS_COL_AMBER_HI, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, m7_back_cb, LV_EVENT_CLICKED, NULL);

    s_path = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_path, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_path, CAS_COL_FG, 0);
    lv_obj_set_pos(s_path, M7_PAD + CAS_CQW(21), M7_PATH_Y + CAS_CQW(1.5));
    lv_obj_clear_flag(s_path, LV_OBJ_FLAG_SCROLLABLE);

    /* 5 行文件/文件夹 */
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, M7_W, M7_ROW_H);
        lv_obj_set_pos(row, M7_PAD, M7_ROWS_Y + i * (M7_ROW_H + M7_ROW_GAP));
        lv_obj_add_style(row, cas_style_groove(), 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, m7_row_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_rows[i] = row;

        /* 标签：文件夹 / 音频 */
        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, s_entries[i].type == FT_DIR ? "文件夹" : "音频");
        lv_obj_set_style_text_font(tag, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(tag,
                                    s_entries[i].type == FT_DIR
                                        ? CAS_COL_AMBER_HI : CAS_COL_STRIPE_Y, 0);
        lv_obj_align(tag, LV_ALIGN_LEFT_MID, 0, 0);

        /* 名称 */
        lv_obj_t *fn = lv_label_create(row);
        lv_label_set_text(fn, s_entries[i].name);
        lv_obj_set_style_text_font(fn, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(fn, CAS_COL_FG, 0);
        lv_obj_set_pos(fn, CAS_CQW(16), M7_ROW_H / 2 - CAS_CQW(2));

        /* 大小/项数 */
        lv_obj_t *fm = lv_label_create(row);
        lv_label_set_text(fm, s_entries[i].meta);
        lv_obj_set_style_text_font(fm, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(fm, CAS_COL_DIM, 0);
        lv_obj_align(fm, LV_ALIGN_RIGHT_MID, -CAS_CQW(1), 0);
    }

    /* 详情面板（米白，选中+展开才显示） */
    s_detail = lv_obj_create(s_root);
    lv_obj_set_size(s_detail, M7_W, M7_DETAIL_H);
    lv_obj_set_pos(s_detail, M7_PAD, M7_DETAIL_Y);
    lv_obj_add_style(s_detail, cas_style_paper(), 0);
    lv_obj_set_style_radius(s_detail, 5, 0);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);
    s_detail_label = lv_label_create(s_detail);
    lv_obj_set_style_text_font(s_detail_label, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_detail_label, CAS_COL_INK, 0);
    lv_obj_center(s_detail_label);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    /* 三键 */
    static const char *const caps[3] = { "打开/播放", "新建目录（示例）", "文件详情" };
    const int kw = (M7_W - 2 * CAS_CQW(1.5)) / 3;
    for (int i = 0; i < M7_KEY_COUNT; ++i) {
        s_keys[i] = cas_metal_key_create(s_root, caps[i], "");
        lv_obj_set_size(s_keys[i], kw, M7_KEYS_H);
        lv_obj_set_pos(s_keys[i], M7_PAD + i * (kw + CAS_CQW(1.5)), M7_KEYS_Y);
        lv_obj_add_event_cb(s_keys[i], m7_key_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    cas_m7_refresh();
    ESP_LOGI(TAG, "M7 created: capbar + breadcrumb + 5 rows + detail + 3 keys");
}

void cas_m7_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    const cas_state_t *st = cas_state_get();

    /* 面包屑：根目录 > 磁带音乐库 [> 新建目录（示例）] */
    char pbuf[CAS_PATH_DEPTH * CAS_PATH_MAX];
    pbuf[0] = '\0';
    for (uint8_t i = 0; i < st->path_len && i < CAS_PATH_DEPTH; ++i) {
        if (i > 0) {
            strncat(pbuf, " > ", sizeof(pbuf) - strlen(pbuf) - 1);
        }
        strncat(pbuf, st->path[i], sizeof(pbuf) - strlen(pbuf) - 1);
    }
    if (s_path != NULL) {
        lv_label_set_text(s_path, pbuf);
    }

    for (int i = 0; i < 5; ++i) {
        if (s_rows[i] == NULL) {
            continue;
        }
        const bool sel = (i == (int)st->sel);
        lv_obj_set_style_border_color(s_rows[i],
                                      sel ? CAS_COL_AMBER : CAS_COL_METAL, 0);
        lv_obj_set_style_border_width(s_rows[i], sel ? 2 : 1, 0);
    }

    /* 详情面板 */
    if (s_detail != NULL) {
        if (st->detail && st->sel >= 0 && st->sel < 5) {
            const cas_fentry_t *f = &s_entries[st->sel];
            char dbuf[96];
            snprintf(dbuf, sizeof(dbuf), "%s：%s · %s（示例）",
                     f->type == FT_DIR ? "文件夹" : "音频", f->name, f->meta);
            if (s_detail_label != NULL) {
                lv_label_set_text(s_detail_label, dbuf);
            }
            lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        }
    }
    cas_metal_key_set_lit(s_keys[M7_DETAIL], st->detail);
}
