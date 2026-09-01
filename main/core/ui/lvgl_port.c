#include "core/ui/lvgl_port.h"
#include "core/hardware/display.h"

#ifdef CONFIG_TOUCH_TEST
#include "core/ui/touch_test.h"
#endif

#include <assert.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;
static TaskHandle_t s_lvgl_task = NULL;   /* LVGL 任务句柄（供深度休眠前挂起） */
static lv_disp_draw_buf_t disp_buf;  // contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;       // contains callback functions
static lv_indev_drv_t indev_drv;     // 触摸输入设备驱动（供 LVGL 控件接收触摸事件）

/* 触摸句柄（触摸异步初始化，就绪前为 NULL）：由触摸初始化完成回调
   （lvgl_port_set_touch_handle）写入，read_cb 读取；就绪前不给触摸数据 */
static esp_lcd_touch_handle_t s_touch_handle = NULL;

#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES / 10)
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

bool lvgl_port_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

#if LCD_BIT_PER_PIXEL == 24
    uint8_t *to = (uint8_t *)color_map;
    uint8_t temp = 0;
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);

    // Special dealing for first pixel
    temp = color_map[0].ch.blue;
    *to++ = color_map[0].ch.red;
    *to++ = color_map[0].ch.green;
    *to++ = temp;
    // Normal dealing for other pixels
    for (int i = 1; i < pixel_num; i++) {
        *to++ = color_map[i].ch.red;
        *to++ = color_map[i].ch.green;
        *to++ = color_map[i].ch.blue;
    }
#endif

    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_rounder_cb(struct _lv_disp_drv_t *disp_driver, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

bool lvgl_port_lock(int timeout_ms)
{
    assert(lvgl_mux && "lvgl_port_init must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    assert(lvgl_mux && "lvgl_port_init must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (lvgl_port_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            // Release the mutex
            lvgl_port_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

lv_disp_drv_t *lvgl_port_get_disp_drv(void)
{
    return &disp_drv;
}

/* 触摸事件订阅：由 indev 触摸轮询统一驱动，避免各功能重复轮询触摸 */
static lvgl_port_touch_cb_t s_touch_cb = NULL;
static void *s_touch_cb_ctx = NULL;

/* 触摸输入设备读取回调：把 esp_lcd_touch 的坐标/按下状态交给 LVGL，
   并转发给注册的触摸事件订阅者（如触摸测试）。
   Y 坐标按比例修正：先乘以 23 再除以 24（触摸面板与屏幕的纵向比例校准）。 */
static void lvgl_touchpad_read_cb(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    (void)indev_driver;

    /* 触摸控制器异步初始化中（句柄未就绪）：不给数据，保持未按下，
       避免操作无效指针；就绪后（lvgl_port_set_touch_handle）自动恢复 */
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = 0;
    data->point.y = 0;
    if (s_touch_handle == NULL) {
        return;
    }

    esp_lcd_touch_handle_t tp = s_touch_handle;
    uint16_t touchpad_x = 0, touchpad_y = 0;
    uint8_t touchpad_cnt = 0;

    esp_lcd_touch_read_data(tp);
    bool touched = esp_lcd_touch_get_coordinates(tp, &touchpad_x, &touchpad_y, NULL, &touchpad_cnt, 1) && (touchpad_cnt > 0);

    /* 以下坐标校准，LVGL 与触摸测试回调共用此值。
       经过调整和优化后点击位置最符合直觉。 */
    touchpad_y = (uint16_t)((uint32_t)touchpad_y * 29 / 30);
    touchpad_x = (uint16_t)((uint32_t)touchpad_x * 29 / 30);
    data->state = touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touchpad_x;
    data->point.y = touchpad_y;

    if (s_touch_cb != NULL) {
        s_touch_cb(touched, touchpad_x, touchpad_y, s_touch_cb_ctx);
    }
}

void lvgl_port_register_touch_cb(lvgl_port_touch_cb_t cb, void *user_ctx)
{
    s_touch_cb = cb;
    s_touch_cb_ctx = user_ctx;
}

/* 设置触摸句柄：触摸异步初始化完成后由上层（main.c 的 ready 回调）调用，
   read_cb 在句柄就绪前返回"未按下"，就绪后自动恢复触摸输入 */
void lvgl_port_set_touch_handle(esp_lcd_touch_handle_t touch_handle)
{
    s_touch_handle = touch_handle;
}

void lvgl_port_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch_handle)
{
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;

    // 设置旋转角度
    // disp_drv.rotated = LV_DISP_ROT_90; // 旋转90度

    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    // 注册触摸输入设备：无条件注册（触摸可能异步初始化，传入句柄为 NULL），
    // read_cb 内部判空，句柄就绪前不给数据；就绪后经 lvgl_port_set_touch_handle 生效
    ESP_LOGI(TAG, "Register touch input device to LVGL");
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touchpad_read_cb;
    indev_drv.user_data = touch_handle;
    lv_indev_drv_register(&indev_drv);

    // 将 LVGL 任务固定到核心 0，避免在多核调度下与其它任务竞争导致 UI 抖动
    xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE,
                            NULL, EXAMPLE_LVGL_TASK_PRIORITY, &s_lvgl_task, 0);

#ifdef CONFIG_TOUCH_TEST
    // 触摸区域测试（Kconfig 控制）：白色圆点跟随手指，抬起后 1s 渐变消失
    touch_test_create();
#endif
}

void lvgl_port_suspend(void)
{
    if (s_lvgl_task != NULL) {
        /* 挂起 LVGL 任务：停止刷屏与触摸轮询。
           深度休眠前调用，避免删除 I2C 驱动后触摸轮询继续报错；
           唤醒后系统重启，无需恢复接口。 */
        vTaskSuspend(s_lvgl_task);
        ESP_LOGI(TAG, "LVGL task suspended");
    }
}
