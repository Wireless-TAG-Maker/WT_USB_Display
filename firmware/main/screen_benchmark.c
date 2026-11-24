#include "screen_benchmark.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "wx101_panel.h"
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
#include "screen_touch.h"
#endif
#if CONFIG_SCREEN_TOUCH_LATENCY_MARKER
#include "latency_marker.h"
#endif

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4 || LVGL_VERSION_PATCH != 0
#error "The recorded benchmark uses exactly LVGL 8.4.0."
#endif
#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP
#error "The P4 bridge expects native RGB565 pixels."
#endif

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 1280
#define DRAW_LINES 64

static const char *TAG = "screen_benchmark";
static lv_disp_draw_buf_t s_draw_buffer;
static lv_disp_drv_t s_display_driver;
static esp_timer_handle_t s_tick_timer;
static int64_t s_started_us;
static bool s_complete;
static void launch_benchmark(void *context);

#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
static lv_indev_drv_t s_touch_driver;
static lv_indev_t *s_touch_input;
static lv_obj_t *s_touch_cursor;
static lv_obj_t *s_touch_status;
static bool s_benchmark_running;
static uint32_t s_touch_clicks;
static screen_touch_point_t s_previous_touch;
static struct {
    uint32_t inputs, changes, read_max_us, gap_max_us;
    uint32_t frames, render_max_ms, flushes, flush_max_us, handler_max_us;
    uint64_t read_us, render_ms, pixels, flush_us;
} s_ui_perf;
static int64_t s_last_input_us;
static bool s_live_coordinate_text = false;

static void touch_frame_monitor(lv_disp_drv_t *driver, uint32_t time_ms, uint32_t pixels)
{
    (void)driver;
    ++s_ui_perf.frames;
    s_ui_perf.render_ms += time_ms;
    s_ui_perf.pixels += pixels;
    if (time_ms > s_ui_perf.render_max_ms) s_ui_perf.render_max_ms = time_ms;
}

static void update_touch_status(void)
{
    if (s_touch_status != NULL) {
        lv_label_set_text_fmt(s_touch_status, "X: %u   Y: %u   %s\nButton clicks: %" PRIu32,
                              s_previous_touch.x, s_previous_touch.y,
                              s_previous_touch.pressed ? "PRESSED" : "RELEASED", s_touch_clicks);
    }
}

static esp_err_t profile_touch_reads(uint32_t bus_hz)
{
    uint32_t minimum = UINT32_MAX, maximum = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < 32; ++i) {
        screen_touch_point_t point;
        int64_t started = esp_timer_get_time();
        esp_err_t err = screen_touch_read_point(&point);
        uint32_t duration = (uint32_t)(esp_timer_get_time() - started);
        if (err != ESP_OK) {
            return err;
        }
        if (duration < minimum) minimum = duration;
        if (duration > maximum) maximum = duration;
        total += duration;
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "TOUCH_READ_PROFILE bus_hz=%" PRIu32
             " samples=32 mean_us=%u min_us=%" PRIu32 " max_us=%" PRIu32,
             bus_hz, (unsigned)(total / 32), minimum, maximum);
    return ESP_OK;
}

static void touch_read_cb(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    static int64_t last_position_log_us;
    screen_touch_point_t point;
    int64_t read_started = esp_timer_get_time();
    esp_err_t err = screen_touch_read_point(&point);
    uint32_t read_us = (uint32_t)(esp_timer_get_time() - read_started);
    ++s_ui_perf.inputs;
    s_ui_perf.read_us += read_us;
    if (read_us > s_ui_perf.read_max_us) s_ui_perf.read_max_us = read_us;
    if (s_last_input_us != 0) {
        uint32_t gap = (uint32_t)(read_started - s_last_input_us);
        if (gap > s_ui_perf.gap_max_us) s_ui_perf.gap_max_us = gap;
    }
    s_last_input_us = read_started;
    if (err != ESP_OK) {
        // A failed read cancels the gesture. It must not synthesize CLICKED
        // from a previously held button when reporting the released state.
        lv_indev_reset(s_touch_input, NULL);
    }
    if (err == ESP_OK && point.pressed) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = s_previous_touch.x;
        data->point.y = s_previous_touch.y;
        data->state = LV_INDEV_STATE_RELEASED;
        point.x = s_previous_touch.x;
        point.y = s_previous_touch.y;
        point.pressed = false;
    }
#if CONFIG_SCREEN_TOUCH_LATENCY_MARKER
    latency_marker_point(point.pressed, point.y);
#endif
    if (point.pressed != s_previous_touch.pressed || point.x != s_previous_touch.x || point.y != s_previous_touch.y) {
        ++s_ui_perf.changes;
        int64_t now = esp_timer_get_time();
        if (point.pressed != s_previous_touch.pressed || now - last_position_log_us >= 200000) {
            ESP_LOGI(TAG, "TOUCH_POINT %s x=%u y=%u transport=%s",
                     point.pressed ? "DOWN" : "UP", point.x, point.y, esp_err_to_name(err));
            last_position_log_us = now;
        }
        s_previous_touch = point;
        if (s_live_coordinate_text) update_touch_status();
    }
}

static void touch_button_cb(lv_event_t *event)
{
    const char *name = lv_event_get_user_data(event);
    ++s_touch_clicks;
    update_touch_status();
    ESP_LOGI(TAG, "TOUCH_UI_CLICK target=%s count=%" PRIu32, name, s_touch_clicks);
#if CONFIG_SCREEN_TOUCH_LATENCY_MARKER
    if (strcmp(name, "Latency test") == 0) latency_marker_begin_flash();
#endif
    if (strcmp(name, "Run benchmark") == 0) {
        // Defer screen deletion until the button's event dispatch has returned.
        lv_async_call(launch_benchmark, NULL);
    }
}

static void make_touch_button(const char *text, lv_align_t align, int x, int y, int width, int height)
{
    lv_obj_t *button = lv_btn_create(lv_scr_act());
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, align, x, y);
    lv_obj_add_event_cb(button, touch_button_cb, LV_EVENT_CLICKED, (void *)text);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

static void start_touch_test(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_24, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "JD9032 + LVGL 8 touch test");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 160);
    s_touch_status = lv_label_create(screen);
    lv_obj_align(s_touch_status, LV_ALIGN_TOP_MID, 0, 230);
    esp_err_t err = screen_touch_start();
    if (err != ESP_OK) {
        lv_label_set_text_fmt(s_touch_status, "Touch initialization failed: %s", esp_err_to_name(err));
        return;
    }
    err = profile_touch_reads(100000);
    if (err != ESP_OK) {
        lv_label_set_text_fmt(s_touch_status, "Touch baseline read failed: %s", esp_err_to_name(err));
        return;
    }
    // JD9366TC datasheet allows up to 400 kHz. Measure both clocks on this board
    // before enabling the faster input path; preserve 100 kHz as a fallback.
    err = screen_touch_set_speed(400000);
    if (err == ESP_OK) err = profile_touch_reads(400000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "400 kHz touch trial failed; restore 100 kHz: %s", esp_err_to_name(err));
        err = screen_touch_set_speed(100000);
        if (err != ESP_OK) {
            lv_label_set_text_fmt(s_touch_status, "Touch clock restore failed: %s", esp_err_to_name(err));
            return;
        }
    }
    (void)screen_touch_describe_coordinate_section();
    lv_indev_drv_init(&s_touch_driver);
    s_touch_driver.type = LV_INDEV_TYPE_POINTER;
    s_touch_driver.read_cb = touch_read_cb;
    s_touch_input = lv_indev_drv_register(&s_touch_driver);
    ESP_ERROR_CHECK(s_touch_input != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    // LVGL 8.4's lv_indev_get_read_timer takes a display, so use the actual
    // input driver's timer instead of that misleadingly named helper.
    lv_timer_set_period(s_touch_input->driver->read_timer, 8);
    lv_timer_set_period(lv_disp_get_default()->refr_timer, 1);
    ESP_LOGI(TAG, "Touch UI periods: input=8 ms refresh=1 ms; coordinate text frozen");
    s_touch_cursor = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_touch_cursor, 80, 80);
    lv_obj_set_style_radius(s_touch_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_touch_cursor, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_touch_cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_touch_cursor, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(s_touch_cursor, 4, 0);
    lv_obj_set_style_translate_x(s_touch_cursor, -40, 0);
    lv_obj_set_style_translate_y(s_touch_cursor, -40, 0);
    lv_obj_clear_flag(s_touch_cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_indev_set_cursor(s_touch_input, s_touch_cursor);
    lv_label_set_text(s_touch_status, "Drag: the red ring is centered on the touch.\nTap the corners and the center button.");
    make_touch_button("TL", LV_ALIGN_TOP_LEFT, 20, 20, 120, 100);
    make_touch_button("TR", LV_ALIGN_TOP_RIGHT, -20, 20, 120, 100);
    make_touch_button("BL", LV_ALIGN_BOTTOM_LEFT, 20, -20, 120, 100);
    make_touch_button("BR", LV_ALIGN_BOTTOM_RIGHT, -20, -20, 120, 100);
#if CONFIG_SCREEN_TOUCH_LATENCY_MARKER
    make_touch_button("Latency test", LV_ALIGN_CENTER, 0, 0, 360, 160);
    ESP_ERROR_CHECK(latency_marker_start());
#else
    make_touch_button("Tap test", LV_ALIGN_CENTER, 0, 0, 360, 160);
#endif
    make_touch_button("Run benchmark", LV_ALIGN_BOTTOM_MID, 0, -180, 360, 100);
    ESP_LOGI(TAG, "LVGL pointer registered; native 800x1280, first finger, no scaling or mirroring");
}
#endif

static void tick_cb(void *context)
{
    (void)context;
    lv_tick_inc(1);
}

static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    // With no DMA2D hook, IDF 6.0.2 copies synchronously and writes the affected
    // PSRAM cache lines back before returning. The draw buffer is then reusable.
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
    int64_t started = esp_timer_get_time();
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(wx101_panel_handle(),
                                             area->x1, area->y1,
                                             area->x2 + 1, area->y2 + 1, pixels));
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
    if (!s_benchmark_running) {
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        ++s_ui_perf.flushes;
        s_ui_perf.flush_us += elapsed;
        if (elapsed > s_ui_perf.flush_max_us) s_ui_perf.flush_max_us = elapsed;
    }
#endif
    lv_disp_flush_ready(driver);
}

static void benchmark_finished(void)
{
    // The full-suite API invokes this before printing the official CSV report.
    // Leave LVGL running to render that report on the screen.
    s_complete = true;
    ESP_LOGI(TAG, "BENCHMARK_FINISHED elapsed_ms=%" PRId64,
             (esp_timer_get_time() - s_started_us) / 1000);
}

static void launch_benchmark(void *context)
{
    (void)context;
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
    s_benchmark_running = true;
#if CONFIG_SCREEN_TOUCH_LATENCY_MARKER
    latency_marker_suspend();
#endif
    s_touch_status = NULL;
    lv_indev_enable(s_touch_input, false);
    lv_obj_add_flag(s_touch_cursor, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_clean(lv_scr_act());
    s_complete = false;
    s_started_us = esp_timer_get_time();
    lv_demo_benchmark_set_finished_cb(benchmark_finished);
    lv_demo_benchmark_set_max_speed(true);
    lv_demo_benchmark();
}

esp_err_t screen_benchmark_run(void)
{
    const size_t buffer_bytes = SCREEN_WIDTH * DRAW_LINES * sizeof(lv_color_t);
    lv_color_t *pixels = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "Cannot allocate %u-byte internal draw buffer", (unsigned)buffer_bytes);
        return ESP_ERR_NO_MEM;
    }
    lv_init();
    lv_disp_draw_buf_init(&s_draw_buffer, pixels, NULL, SCREEN_WIDTH * DRAW_LINES);
    lv_disp_drv_init(&s_display_driver);
    s_display_driver.hor_res = SCREEN_WIDTH;
    s_display_driver.ver_res = SCREEN_HEIGHT;
    s_display_driver.draw_buf = &s_draw_buffer;
    s_display_driver.flush_cb = flush_cb;
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
    s_display_driver.monitor_cb = touch_frame_monitor;
#endif
    if (lv_disp_drv_register(&s_display_driver) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 1000));

#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
    ESP_LOGI(TAG, "LVGL 8.4.0 touch test; benchmark starts with the on-screen button");
    start_touch_test();
#else
    ESP_LOGI(TAG, "LVGL %d.%d.%d official benchmark; 800x1280 RGB565 -> RGB888",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    ESP_LOGI(TAG, "Software rendering; one internal 800x64 draw buffer (%u bytes); synchronous CPU flush",
             (unsigned)buffer_bytes);
    ESP_LOGI(TAG, "One PSRAM framebuffer; no VSYNC throttling or tearing protection; official max_speed=true");
    ESP_LOGI(TAG, "Heap free: internal=%u PSRAM=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    launch_benchmark(NULL);
#endif

    uint32_t last_refresh = wx101_panel_refresh_count();
    int64_t last_health_us = esp_timer_get_time();
    while (1) {
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
        int64_t handler_started = esp_timer_get_time();
#endif
        uint32_t delay_ms = lv_timer_handler();
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
        if (!s_benchmark_running) {
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - handler_started);
            if (elapsed > s_ui_perf.handler_max_us) s_ui_perf.handler_max_us = elapsed;
        }
        if (s_complete && s_benchmark_running) {
            s_benchmark_running = false;
            lv_indev_reset(s_touch_input, NULL);
            lv_indev_enable(s_touch_input, true);
            lv_obj_clear_flag(s_touch_cursor, LV_OBJ_FLAG_HIDDEN);
            lv_disp_get_default()->driver->monitor_cb = touch_frame_monitor;
            s_last_input_us = 0;
            ESP_LOGI(TAG, "Touch re-enabled; the benchmark result table can be scrolled");
        }
#endif
        if (delay_ms > 5) delay_ms = 5;
        if (delay_ms < 1) delay_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_health_us >= 5000000) {
#if CONFIG_SCREEN_ENABLE_TOUCH_PROBE
            if (!s_benchmark_running) {
                ESP_LOGI(TAG, "UI_PERF text_live=%u inputs=%" PRIu32 " changes=%" PRIu32
                         " read_mean_us=%u read_max_us=%" PRIu32 " gap_max_us=%" PRIu32
                         " frames=%" PRIu32 " render_mean_ms=%u render_max_ms=%" PRIu32
                         " pixels_per_frame=%u flush_mean_us=%u flush_max_us=%" PRIu32
                         " handler_max_us=%" PRIu32,
                         (unsigned)s_live_coordinate_text, s_ui_perf.inputs, s_ui_perf.changes,
                         s_ui_perf.inputs ? (unsigned)(s_ui_perf.read_us / s_ui_perf.inputs) : 0,
                         s_ui_perf.read_max_us, s_ui_perf.gap_max_us, s_ui_perf.frames,
                         s_ui_perf.frames ? (unsigned)(s_ui_perf.render_ms / s_ui_perf.frames) : 0,
                         s_ui_perf.render_max_ms,
                         s_ui_perf.frames ? (unsigned)(s_ui_perf.pixels / s_ui_perf.frames) : 0,
                         s_ui_perf.flushes ? (unsigned)(s_ui_perf.flush_us / s_ui_perf.flushes) : 0,
                         s_ui_perf.flush_max_us, s_ui_perf.handler_max_us);
                memset(&s_ui_perf, 0, sizeof(s_ui_perf));
            }
#endif
            uint32_t refresh = wx101_panel_refresh_count();
            ESP_LOGI(TAG, "Health: complete=%u host_refresh=%" PRIu32
                     " internal_free=%u internal_min=%u stack_free=%u",
                     (unsigned)s_complete, refresh - last_refresh,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_refresh = refresh;
            last_health_us = now_us;
        }
    }
}
