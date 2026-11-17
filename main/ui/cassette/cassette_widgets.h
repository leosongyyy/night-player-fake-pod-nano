#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "ui/cassette/cassette_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_widgets.h
 * @brief 7 模块复用的拟物控件工厂（推子/琴键/LED/拨杆/旋钮/分段/翻页卡/螺丝/三色条/容量条）
 *
 * 全部用 LVGL 9 控件 + 样式绘制，零新增位图（架构 §1.1）。样式只从
 * cassette_theme.h 的 15 色 Palette 取色，任何模块不得硬编码近似色。
 * 控件状态由调用方保存在 cas_state_t；编程式 set 之后调用方须手动刷新
 * 关联 label（LVGL 的 set 不改变业务状态，见 §7.5）。
 */

typedef enum {
    CAS_FADER_BRI  = 0,   /* 亮度：黄→橙渐变填充 */
    CAS_FADER_VOL  = 1,   /* 音量：琥珀填充 */
    CAS_FADER_PROG = 2,   /* 播放进度：较矮、圆形亮色 thumb */
} cas_fader_type_t;

typedef struct {
    lv_obj_t       *root;     /* lv_slider 本体 */
    cas_fader_type_t type;
    uint8_t         value;    /* 0~100 */
} cas_fader_t;

/** 推子：默认宽 CAS_CQW(90)、高按 type；调用方可改 root 尺寸后重设 value 触发刷新 */
cas_fader_t *cas_fader_create(lv_obj_t *parent, cas_fader_type_t type);
void cas_fader_set_value(cas_fader_t *f, uint8_t value);  /* 编程式设置 + 手动刷新 */
uint8_t cas_fader_get_value(const cas_fader_t *f);
void cas_fader_set_on_change(cas_fader_t *f, lv_event_cb_t cb, void *user_data);

/** 金属琴键：cap 为大符号（◄◄/❚❚/►/■/►►），label 为中文小字（快退/暂停…） */
lv_obj_t *cas_metal_key_create(lv_obj_t *parent, const char *cap,
                               const char *label);
void cas_metal_key_set_lit(lv_obj_t *key, bool lit);   /* 点亮态 = LV_STATE_CHECKED */

/** LED 圆点：关=深槽色 / 开=琥珀发光 */
lv_obj_t *cas_led_create(lv_obj_t *parent);
void cas_led_set_on(lv_obj_t *led, bool on);

/** 拨杆：轨道 + 滑块，set_on 只改位置（P0 无动画） */
lv_obj_t *cas_lever_create(lv_obj_t *parent);
void cas_lever_set_on(lv_obj_t *lever, bool on);

/** 旋钮：圆盘 + 指示条，angle10 单位 0.1°（LVGL 9.4 transform_rotation） */
lv_obj_t *cas_knob_create(lv_obj_t *parent);
void cas_knob_set_angle(lv_obj_t *knob, int32_t angle10);

/** 分段按钮：labels 为以 NULL 结尾的字符串数组 */
lv_obj_t *cas_seg_create(lv_obj_t *parent, const char *const labels[]);
void cas_seg_set_active(lv_obj_t *seg, uint8_t idx);

/** 翻页卡：上下半区 + 中缝 + 左右铆钉；digit 为单个数字字符 */
lv_obj_t *cas_flip_card_create(lv_obj_t *parent);
void cas_flip_card_set_digit(lv_obj_t *card, char digit);
/** 翻页卡写入两位文本（时钟时/分/秒，如 "10"/"24"）；最多 2 字符 */
void cas_flip_card_set_text(lv_obj_t *card, const char *text);

/** 十字螺丝：deg10 = 旋转角 ×10；供 T04/T05 各屏复用 */
lv_obj_t *cas_screw_create(lv_obj_t *parent, int32_t deg10);

/** 三色条（黄/橙/红）：按给定宽高等比三等分 */
lv_obj_t *cas_stripes_create(lv_obj_t *parent, int32_t w, int32_t h);

/** 容量条：三色分段，pct 0~100 决定填充到第几段 */
lv_obj_t *cas_capbar_create(lv_obj_t *parent, int32_t w, int32_t h);
void cas_capbar_set_pct(lv_obj_t *bar, uint8_t pct);

#ifdef __cplusplus
}
#endif
