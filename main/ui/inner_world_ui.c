#include "ui/inner_world_ui.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/network/wifi_time_sync.h"
#include "core/storage/config_store.h"
#include "core/system/usb_msc.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/power_service.h"
#include "ui/time_date_popup.h"
#include "ui/time_sync_ui.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

/* idfc quickconfig（`idfc build -qc <名字>` 生成，声明在 idfc_buildconfig.json）：
   临时调试开关不走 Kconfig，启用/关闭都不触发 reconfigure。 */
#if defined(__has_include)
#  if __has_include("idfc_quickconfig.h")
#    include "idfc_quickconfig.h"
#  endif
#endif
#ifndef QCONF_INNER_USB_AUTOSTART
#define QCONF_INNER_USB_AUTOSTART 0
#endif

static const char *TAG = "inner_world";

/* ------------------------------------------------------------------ */
/* 布局与配色                                                           */
/* ------------------------------------------------------------------ */

#define IW_W  EXAMPLE_LCD_H_RES
#define IW_H  EXAMPLE_LCD_V_RES

#define IW_BG          lv_color_hex(0x000000)   /* 全屏底色：纯黑（不透明覆盖层） */
#define IW_CARD        lv_color_hex(0x1E1E1E)   /* 功能行卡片：深灰 */
#define IW_CARD_PRESS  lv_color_hex(0x2C2C2C)   /* 行按下反馈 */
#define IW_ACCENT      lv_color_hex(0xFFC000)   /* 强调色（标题/主按钮）：黄 */
#define IW_TEXT        lv_color_white()
#define IW_DIM         lv_palette_main(LV_PALETTE_GREY)
#define IW_OK          lv_color_hex(0x7DE08A)
#define IW_ERR         lv_color_hex(0xFF6B6B)

/* 功能行尺寸与位置（460×460 屏幕；标题下没有副标题，列表整体略上移） */
#define IW_ROW_W       392
#define IW_ROW_H       86
#define IW_ROW1_Y      132
#define IW_ROW2_Y      230

/* 启动 USB 栈的任务参数：栈必须放内部 RAM（SDMMC/TinyUSB 初始化都要内部
   DMA 缓冲，PSRAM 栈任务在这类深调用里容易踩关缓存断言） */
#define IW_USB_TASK_STACK  (5 * 1024)
#define IW_USB_TASK_PRIO   5

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_list_root = NULL;      /* 里世界主界面（列表层） */
static lv_obj_t *s_sync_status = NULL;    /* 「WiFi对时」行右侧状态 */
static lv_obj_t *s_usb_status = NULL;     /* 「USB传歌」行右侧状态 */

static lv_obj_t *s_usb_root = NULL;       /* USB 模式全屏层（惰性创建） */
static lv_obj_t *s_usb_state = NULL;      /* USB 模式的状态主文字 */
static lv_obj_t *s_usb_detail = NULL;     /* USB 模式的容量/提示文字 */
static lv_timer_t *s_usb_poll_timer = NULL;

static lv_timer_t *s_tick_timer = NULL;   /* 列表状态每秒刷新 */

/* 本次里世界运行期间上一次对时的结果文案（空 = 还没对过时） */
static char s_sync_state[24] = "";

/* USB 启动任务是否在跑（防止连点重复起任务） */
static volatile bool s_usb_starting = false;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

/* 把 SD 卡容量格式化成 "32.0 GB" / "512 MB" */
static void format_capacity(uint64_t bytes, char *buf, size_t size)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(buf, size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (uint64_t)1024 * 1024) {
        snprintf(buf, size, "%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
    } else {
        snprintf(buf, size, "容量未知");
    }
}

/* ------------------------------------------------------------------ */
/* 列表层：功能行                                                       */
/* ------------------------------------------------------------------ */

/* 一张功能行卡片：左侧图标 + 标题，右侧状态文字 */
static lv_obj_t *iw_row_create(lv_obj_t *parent, int32_t y, const char *icon,
                               const char *title, lv_obj_t **status_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, IW_ROW_W, IW_ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, IW_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, IW_CARD_PRESS, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, UI_ICON_TEXT_FONT, 0);
    lv_obj_set_style_text_color(ic, IW_ACCENT, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, IW_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 64, 0);

    if (status_out != NULL) {
        lv_obj_t *st = lv_label_create(row);
        lv_label_set_text(st, "");
        lv_obj_set_style_text_font(st, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(st, IW_DIM, 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, -22, 0);
        *status_out = st;
    }
    return row;
}

/* 刷新列表两行的右侧状态（LVGL 上下文） */
static void iw_update_status(void)
{
    char buf[40];

    if (s_sync_status != NULL) {
        if (s_sync_state[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", s_sync_state);
        } else {
            rtc_time_t rt;
            buf[0] = '\0';
            if (rtc_is_ready() && rtc_get_time(&rt) == ESP_OK) {
                char ts[16];
                time_date_format_time(&rt, ts, sizeof(ts));
                /* 只留 HH:MM，秒数对"看个时间"没意义 */
                snprintf(buf, sizeof(buf), "%.5s", ts);
            }
        }
        lv_label_set_text(s_sync_status, buf);
    }

    if (s_usb_status != NULL) {
        lv_label_set_text(s_usb_status,
                          sd_card_is_mounted() ? "可传歌" : "无 SD 卡");
    }
}

/* 每秒刷新一次（只更新列表层；对时/传歌界面盖住时也不费什么） */
static void iw_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    iw_update_status();
}

/* ------------------------------------------------------------------ */
/* 回到表世界                                                           */
/* ------------------------------------------------------------------ */

static void iw_restart_cb(lv_event_t *e)
{
    (void)e;
    /* 分区位是一次性请求：bootloader 进 ota_0 时已清 0，任何重启都会回
       factory 里的表世界固件；这里不需要再写启动分区（也不碰 otadata）。 */
    ESP_LOGI(TAG, "回到表世界：重启（分区位已清 → 启动 factory 播放器）");
    vTaskDelay(pdMS_TO_TICKS(120));   /* 让日志先出去 */
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* WiFi 对时                                                            */
/* ------------------------------------------------------------------ */

/* 对时结束（结果已展示完 / 用户点关闭）→ 回到里世界列表并记录结果 */
static void iw_sync_done(wifi_time_sync_result_t result)
{
    /* 对时进度层是全屏不透明层，必须显式隐藏才能看到列表 */
    time_sync_ui_dismiss();

    if (result == WIFI_TIME_SYNC_OK) {
        char ts[16] = "";
        rtc_time_t rt;
        if (rtc_is_ready() && rtc_get_time(&rt) == ESP_OK) {
            snprintf(ts, sizeof(ts), "%02u:%02u", (unsigned)rt.hour,
                     (unsigned)rt.minute);
        }
        snprintf(s_sync_state, sizeof(s_sync_state), "成功 %s", ts);
        ui_toast_show("对时成功");
    } else if (result == WIFI_TIME_SYNC_CANCELLED) {
        snprintf(s_sync_state, sizeof(s_sync_state), "已取消");
        ui_toast_show("对时已取消");
    } else {
        snprintf(s_sync_state, sizeof(s_sync_state), "失败");
        ui_toast_show(wifi_time_sync_result_text(result));
    }

    iw_update_status();
    inner_world_ui_show();
}

static void iw_sync_row_cb(lv_event_t *e)
{
    (void)e;
    if (time_sync_ui_is_visible()) {
        return;
    }
    ESP_LOGI(TAG, "进入 WiFi 对时");
    if (!time_sync_ui_start()) {
        ui_toast_show("对时启动失败：内存不足");
    }
}

/* ------------------------------------------------------------------ */
/* USB 传歌                                                             */
/* ------------------------------------------------------------------ */

/* USB 模式界面（惰性创建）：进入后不再退出，只能重启回表世界 */
static void usb_ui_create(void)
{
    if (s_usb_root != NULL) {
        return;
    }

    s_usb_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_usb_root, IW_W, IW_H);
    lv_obj_set_pos(s_usb_root, 0, 0);
    lv_obj_set_style_bg_color(s_usb_root, IW_BG, 0);
    lv_obj_set_style_bg_opa(s_usb_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_usb_root, 0, 0);
    lv_obj_set_style_radius(s_usb_root, 0, 0);
    lv_obj_set_style_pad_all(s_usb_root, 0, 0);
    lv_obj_clear_flag(s_usb_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_usb_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_usb_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_usb_root);
    lv_label_set_text(title, "USB传歌");
    lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title, IW_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = lv_label_create(s_usb_root);
    lv_label_set_text(sub, "SD 卡已作为 U 盘连接到电脑");
    lv_obj_set_style_text_font(sub, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(sub, IW_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 60);

    /* 中间信息卡：状态 + 容量 + 两条操作提示 */
    lv_obj_t *card = lv_obj_create(s_usb_root);
    lv_obj_set_size(card, 400, 240);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_color(card, IW_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    s_usb_state = lv_label_create(card);
    lv_label_set_text(s_usb_state, "正在启动 USB…");
    lv_obj_set_style_text_font(s_usb_state, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_usb_state, IW_ACCENT, 0);
    lv_obj_align(s_usb_state, LV_ALIGN_TOP_MID, 0, 16);

    s_usb_detail = lv_label_create(card);
    lv_label_set_text(s_usb_detail, "");
    lv_obj_set_style_text_font(s_usb_detail, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_usb_detail, IW_DIM, 0);
    lv_obj_set_style_text_align(s_usb_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_usb_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_usb_detail, 352);
    lv_obj_align(s_usb_detail, LV_ALIGN_TOP_MID, 0, 54);

    /* 两条操作提示：各占**一整行、不换行**（label 不设宽度 → 按文本自适应，
       用 TOP_MID 居中）。原先把长句设成 352px 换行，折行后第二行会压到卡片
       底部、被「回到表世界」按钮盖住（2026-11 用户截图反馈）。 */
    lv_obj_t *hint1 = lv_label_create(card);
    lv_label_set_text(hint1, "若电脑未识别请把插头翻面");
    lv_obj_set_style_text_font(hint1, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(hint1, IW_DIM, 0);
    lv_obj_align(hint1, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *hint2 = lv_label_create(card);
    lv_label_set_text(hint2, "传完请安全弹出再点下方按钮");
    lv_obj_set_style_text_font(hint2, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(hint2, IW_DIM, 0);
    lv_obj_align(hint2, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *btn = lv_obj_create(s_usb_root);
    lv_obj_set_size(btn, 260, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(btn, IW_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_YELLOW, 1),
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, iw_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "回到表世界");
    lv_obj_set_style_text_font(btn_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_black(), 0);
    lv_obj_center(btn_lbl);

    ESP_LOGI(TAG, "USB 模式界面已创建");
}

/* USB 模式状态轮询：电脑挂载/卸载时更新主状态文字 */
static void iw_usb_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_usb_state == NULL) {
        return;
    }
    if (usb_msc_host_mounted()) {
        lv_label_set_text(s_usb_state, "电脑已连接，可以开始传歌");
        lv_obj_set_style_text_color(s_usb_state, IW_OK, 0);
    } else {
        lv_label_set_text(s_usb_state, "等待电脑识别…");
        lv_obj_set_style_text_color(s_usb_state, IW_ACCENT, 0);
    }
}

/* USB 栈启动完成（LVGL 上下文，持锁） */
static void iw_usb_start_done(usb_msc_result_t result)
{
    s_usb_starting = false;

    if (result != USB_MSC_OK) {
        ESP_LOGE(TAG, "USB 传歌启动失败：%s", usb_msc_result_text(result));
        ui_toast_show(usb_msc_result_text(result));
        if (s_usb_root != NULL) {
            lv_obj_add_flag(s_usb_root, LV_OBJ_FLAG_HIDDEN);
        }
        inner_world_ui_show();
        return;
    }

    /* 成功：USB 模式不退出，且**禁止自动息屏**（息屏后会再触发"超时关机"，
       而 USB 传歌通常没有触摸操作）。传歌期间允许屏幕一直亮着。 */
    power_service_set_hold_awake(true);

    /* 成功：显示容量，切换到"等待电脑"状态，并开始轮询挂载状态 */
    char cap[24];
    format_capacity(usb_msc_capacity_bytes(), cap, sizeof(cap));
    if (s_usb_detail != NULL) {
        lv_label_set_text_fmt(s_usb_detail, "容量 %s", cap);
    }
    lv_label_set_text(s_usb_state, "等待电脑识别…");
    lv_obj_set_style_text_color(s_usb_state, IW_ACCENT, 0);

    if (s_usb_poll_timer == NULL) {
        s_usb_poll_timer = lv_timer_create(iw_usb_poll_cb, 500, NULL);
    }
    ESP_LOGI(TAG, "USB 传歌界面就绪（容量 %s）", cap);
}

/* 启动 USB 栈的任务：SD 初始化 + TinyUSB 安装都不能在 LVGL 回调里同步做 */
static void iw_usb_start_task(void *arg)
{
    (void)arg;
    /* 先让"正在启动 USB…"这一帧画出来，再去做阻塞的初始化 */
    vTaskDelay(pdMS_TO_TICKS(80));

    usb_msc_result_t result = usb_msc_start();

    if (lvgl_port_lock(-1)) {
        iw_usb_start_done(result);
        lvgl_port_unlock();
    } else {
        s_usb_starting = false;
    }
    vTaskDelete(NULL);
}

static void iw_usb_row_cb(lv_event_t *e)
{
    (void)e;

    usb_ui_create();
    lv_obj_move_foreground(s_usb_root);
    lv_obj_clear_flag(s_usb_root, LV_OBJ_FLAG_HIDDEN);

    /* 已经在跑（理论上不会走到，USB 模式不退出）：直接显示界面即可 */
    if (usb_msc_is_active()) {
        iw_usb_poll_cb(NULL);
        return;
    }
    if (s_usb_starting) {
        return;
    }
    s_usb_starting = true;

    if (s_usb_state != NULL) {
        lv_label_set_text(s_usb_state, "正在启动 USB…");
        lv_obj_set_style_text_color(s_usb_state, IW_ACCENT, 0);
    }
    if (s_usb_detail != NULL) {
        lv_label_set_text(s_usb_detail, "");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(iw_usb_start_task, "usb_msc_start",
                                            IW_USB_TASK_STACK, NULL,
                                            IW_USB_TASK_PRIO, NULL, 1);
    if (ok != pdPASS) {
        s_usb_starting = false;
        ESP_LOGE(TAG, "创建 USB 启动任务失败");
        ui_toast_show("内存不足，无法启动 USB 传歌");
        lv_obj_add_flag(s_usb_root, LV_OBJ_FLAG_HIDDEN);
        inner_world_ui_show();
    }
    ESP_LOGI(TAG, "进入 USB 传歌（启动中）");
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

void inner_world_ui_create(void)
{
    if (s_list_root != NULL) {
        return;
    }

    /* 全屏不透明层 → 挂屏幕层（与 time_sync_ui 同一约定：覆盖判定要靠
       屏幕子对象树，挂 lv_layer_top 会让下层每帧重绘） */
    s_list_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_list_root, IW_W, IW_H);
    lv_obj_set_pos(s_list_root, 0, 0);
    lv_obj_set_style_bg_color(s_list_root, IW_BG, 0);
    lv_obj_set_style_bg_opa(s_list_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list_root, 0, 0);
    lv_obj_set_style_radius(s_list_root, 0, 0);
    lv_obj_set_style_pad_all(s_list_root, 0, 0);
    lv_obj_clear_flag(s_list_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_list_root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(s_list_root);
    lv_label_set_text(title, "里世界");
    lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title, IW_ACCENT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    /* WiFi对时：右侧显示上次对时结果；还没对过时显示当前时间 */
    lv_obj_t *sync_row = iw_row_create(s_list_root, IW_ROW1_Y, LV_SYMBOL_WIFI,
                                       "WiFi对时", &s_sync_status);
    lv_obj_add_flag(sync_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sync_row, iw_sync_row_cb, LV_EVENT_CLICKED, NULL);

    /* USB传歌：右侧显示 SD 卡是否就绪 */
    lv_obj_t *usb_row = iw_row_create(s_list_root, IW_ROW2_Y, LV_SYMBOL_USB,
                                      "USB传歌", &s_usb_status);
    lv_obj_add_flag(usb_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(usb_row, iw_usb_row_cb, LV_EVENT_CLICKED, NULL);

    /* 底部：回到表世界（分区位已被 bootloader 消费 → 重启即回 factory） */
    lv_obj_t *btn = lv_obj_create(s_list_root);
    lv_obj_set_size(btn, 260, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_bg_color(btn, IW_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, IW_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, iw_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "回到表世界");
    lv_obj_set_style_text_font(btn_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(btn_lbl, IW_TEXT, 0);
    lv_obj_center(btn_lbl);

    /* 对时结束后的去处：回列表并记录结果（见 iw_sync_done） */
    time_sync_ui_set_done_cb(iw_sync_done);

    iw_update_status();
    s_tick_timer = lv_timer_create(iw_tick_cb, 1000, NULL);

    ESP_LOGI(TAG, "里世界主界面已创建");

#if QCONF_INNER_USB_AUTOSTART
    /* 【调试】无人值守验证 USB 传歌：里世界起来后直接进 USB 模式（等价于
       用户点「USB传歌」）。⚠ 仅调试用：进入后不能退回列表，只能重启。 */
    ESP_LOGW(TAG, "INNER_USB_AUTOSTART: 自动进入 USB 传歌");
    iw_usb_row_cb(NULL);
#endif
}

void inner_world_ui_show(void)
{
    if (s_list_root == NULL) {
        return;
    }
    lv_obj_move_foreground(s_list_root);
    lv_obj_clear_flag(s_list_root, LV_OBJ_FLAG_HIDDEN);
    iw_update_status();
}

bool inner_world_ui_is_visible(void)
{
    return s_list_root != NULL &&
           !lv_obj_has_flag(s_list_root, LV_OBJ_FLAG_HIDDEN) &&
           !time_sync_ui_is_visible() &&
           (s_usb_root == NULL || lv_obj_has_flag(s_usb_root, LV_OBJ_FLAG_HIDDEN));
}
