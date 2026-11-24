#include "latency_marker.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "lvgl.h"

#define MARKER_LED_GPIO 51
#define MARKER_CENTER_Y 640
#define MARKER_FLASH_PERIOD_MS 500
#define MARKER_FLASH_EDGES 20

static const char *TAG = "latency_marker";
static led_strip_handle_t s_strip;
static lv_obj_t *s_square;
static lv_timer_t *s_flash_timer;
static bool s_led_on;
static bool s_flash_on;
static bool s_flashing;
static bool s_suspended;
static unsigned s_flash_sequence;

static esp_err_t set_led(bool on)
{
    if (on == s_led_on) {
        return ESP_OK;
    }
    uint32_t level = on ? 64 : 0;
    esp_err_t err = led_strip_set_pixel(s_strip, 0, level, level, level);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err == ESP_OK) {
        s_led_on = on;
    }
    return err;
}

static void set_flash_state(bool on, const char *phase)
{
    int64_t requested_us = esp_timer_get_time();
    // The LCD property is changed first. Actual scanout happens independently;
    // the camera compares its visible edge against the physical LED's edge.
    lv_obj_set_style_bg_color(s_square, on ? lv_color_white() : lv_color_black(), 0);
    esp_err_t err = set_led(on);
    ESP_LOGI(TAG, "MARKER mode=FLASH phase=%s seq=%u state=%u t_us=%" PRId64 " led=%s",
             phase, s_flash_sequence, (unsigned)on, requested_us, esp_err_to_name(err));
}

static void flash_cb(lv_timer_t *timer)
{
    s_flash_on = !s_flash_on;
    ++s_flash_sequence;
    set_flash_state(s_flash_on, "edge");
    if (s_flash_sequence == MARKER_FLASH_EDGES) {
        lv_timer_pause(timer);
        s_flashing = false;
        ESP_LOGI(TAG, "MARKER mode=TOUCH t_us=%" PRId64, esp_timer_get_time());
    }
}

esp_err_t latency_marker_start(void)
{
    if (s_strip != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Official WT9932P4C61-TINY BSP: one WS2812, GPIO51, GRB, non-inverted.
    led_strip_config_t strip_config = {
        .strip_gpio_num = MARKER_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        return err;
    }
    err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return err;
    }
    s_led_on = false;

    s_square = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_square);
    lv_obj_set_size(s_square, 160, 160);
    lv_obj_align(s_square, LV_ALIGN_TOP_MID, 0, 360);
    lv_obj_set_style_bg_opa(s_square, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_square, lv_color_black(), 0);
    lv_obj_clear_flag(s_square, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *center_line = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(center_line);
    lv_obj_set_pos(center_line, 0, MARKER_CENTER_Y);
    lv_obj_set_size(center_line, 800, 2);
    lv_obj_set_style_bg_opa(center_line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(center_line, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_clear_flag(center_line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_suspended = false;
    ESP_LOGI(TAG, "MARKER ready: GPIO51 WS2812, LCD square y=360, touch threshold y=640");
    return ESP_OK;
}

void latency_marker_begin_flash(void)
{
    if (s_strip == NULL || s_suspended) {
        return;
    }
    if (s_flash_timer == NULL) {
        s_flash_timer = lv_timer_create(flash_cb, MARKER_FLASH_PERIOD_MS, NULL);
        if (s_flash_timer == NULL) {
            ESP_LOGE(TAG, "MARKER timer allocation failed");
            return;
        }
    }
    s_flashing = true;
    s_flash_on = false;
    s_flash_sequence = 0;
    set_flash_state(false, "baseline");
    // First edge is at +500 ms; edge 20 is at +10 s, excluding scheduler jitter.
    lv_timer_reset(s_flash_timer);
    lv_timer_resume(s_flash_timer);
}

void latency_marker_point(bool pressed, uint16_t y)
{
    if (s_strip == NULL || s_suspended || s_flashing) {
        return;
    }
    bool on = pressed && y >= MARKER_CENTER_Y;
    if (on != s_led_on) {
        int64_t requested_us = esp_timer_get_time();
        esp_err_t err = set_led(on);
        ESP_LOGI(TAG, "MARKER mode=TOUCH state=%u pressed=%u y=%u t_us=%" PRId64 " led=%s",
                 (unsigned)on, (unsigned)pressed, y, requested_us, esp_err_to_name(err));
    }
}

void latency_marker_suspend(void)
{
    s_suspended = true;
    s_flashing = false;
    if (s_flash_timer != NULL) {
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
    }
    if (s_strip != NULL) {
        esp_err_t err = set_led(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MARKER suspend LED clear: %s", esp_err_to_name(err));
        }
    }
    s_square = NULL;
}
