// SPDX-License-Identifier: GPL-2.0
/*
 * WX101CH021I-40Z bring-up using the ESP-IDF 6.0.2 native DSI driver.
 * Panel initialization data and its upstream notices are in wx101_init.h.
 */
#include "wx101_panel.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/mipi_dsi_host_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "wx101_init.h"
#include "sdkconfig.h"

#define WX101_WIDTH 800U
#define WX101_HEIGHT 1280U
#if CONFIG_SCREEN_ENABLE_BENCHMARK
#define WX101_BYTES_PER_PIXEL 2U
#define WX101_INPUT_FORMAT LCD_COLOR_FMT_RGB565
#else
#define WX101_BYTES_PER_PIXEL 3U
#define WX101_INPUT_FORMAT LCD_COLOR_FMT_RGB888
#endif
#define WX101_FRAME_BYTES ((size_t)WX101_WIDTH * WX101_HEIGHT * WX101_BYTES_PER_PIXEL)
#define WX101_COMMAND_TIMEOUT_US 200000
// Match the upstream write-only DCS sequence without ACK/BTA turnaround.
#define WX101_REQUEST_COMMAND_ACK false

static const char *TAG = "wx101_panel";
static esp_ldo_channel_handle_t s_phy_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_command_io;
static esp_lcd_panel_handle_t s_dpi_panel;
static DRAM_ATTR volatile uint32_t s_refresh_count;

#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
#define WX101_FB_COUNT 3
enum { FB_FREE, FB_ACQUIRED, FB_PENDING, FB_FRONT };
static void *s_framebuffers[WX101_FB_COUNT];
static DRAM_ATTR volatile uint8_t s_fb_states[WX101_FB_COUNT];
static DRAM_ATTR volatile int s_front_index;
static DRAM_ATTR volatile int s_pending_index = -1;
static DRAM_ATTR volatile uint32_t s_pending_due;
static DRAM_ATTR volatile uint32_t s_present_count;
static DRAM_ATTR int64_t s_last_vsync_us;
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_free_frames;
static SemaphoreHandle_t s_vsync;
static SemaphoreHandle_t s_committed;
static SemaphoreHandle_t s_present_lock;
#else
#define WX101_FB_COUNT 1
#endif

static esp_err_t wait_command_fifos(uint8_t command)
{
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    int64_t deadline = esp_timer_get_time() + WX101_COMMAND_TIMEOUT_US;
    while (!mipi_dsi_host_ll_gen_is_cmd_fifo_empty(host) ||
           !mipi_dsi_host_ll_gen_is_write_fifo_empty(host)) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "DCS 0x%02x transport timeout: fifo=0x%08" PRIx32
                     ", phy=0x%08" PRIx32,
                     command, host->cmd_pkt_status.val, host->phy_status.val);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t send_command(uint8_t command, const void *data, size_t size)
{
    // This table's largest packet is 15 bytes, four payload words. Start each
    // bounded packet with empty FIFOs so the SDK's unbounded FIFO loops cannot
    // encounter a queue left full by an earlier command.
    if (size > 14) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = wait_command_fifos(command);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_io_tx_param(s_command_io, command, data, size);
    return err == ESP_OK ? wait_command_fifos(command) : err;
}

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel,
                                     esp_lcd_dpi_panel_event_data_t *event_data,
                                     void *user_context)
{
    (void)panel;
    (void)event_data;
    (void)user_context;
    ++s_refresh_count;
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_frame_lock);
    s_last_vsync_us = esp_timer_get_time();
    if (s_pending_index >= 0 && (int32_t)(s_refresh_count - s_pending_due) >= 0) {
        uint8_t released = (uint8_t)s_front_index;
        s_fb_states[released] = FB_FREE;
        s_front_index = s_pending_index;
        s_fb_states[s_front_index] = FB_FRONT;
        s_pending_index = -1;
        ++s_present_count;
        xQueueSendFromISR(s_free_frames, &released, &woken);
        xSemaphoreGiveFromISR(s_committed, &woken);
    }
    portEXIT_CRITICAL_ISR(&s_frame_lock);
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
#else
    return false;
#endif
}

static void release_resources(void)
{
    esp_err_t err;

    if (s_dpi_panel != NULL) {
        err = esp_lcd_panel_del(s_dpi_panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release DPI panel failed: %s", esp_err_to_name(err));
            return;
        }
        s_dpi_panel = NULL;
    }
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    if (s_free_frames != NULL) { vQueueDelete(s_free_frames); s_free_frames = NULL; }
    if (s_vsync != NULL) { vSemaphoreDelete(s_vsync); s_vsync = NULL; }
    if (s_committed != NULL) { vSemaphoreDelete(s_committed); s_committed = NULL; }
    if (s_present_lock != NULL) { vSemaphoreDelete(s_present_lock); s_present_lock = NULL; }
#endif
    if (s_command_io != NULL) {
        err = esp_lcd_panel_io_del(s_command_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release command IO failed: %s", esp_err_to_name(err));
            return;
        }
        s_command_io = NULL;
    }
    if (s_dsi_bus != NULL) {
        err = esp_lcd_del_dsi_bus(s_dsi_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release DSI bus failed: %s", esp_err_to_name(err));
            return;
        }
        s_dsi_bus = NULL;
    }
    if (s_phy_ldo != NULL) {
        err = esp_ldo_release_channel(s_phy_ldo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release PHY LDO failed: %s", esp_err_to_name(err));
            return;
        }
        s_phy_ldo = NULL;
    }
}

static esp_err_t fill_color_bars(void)
{
    void *raw_frame_buffer = NULL;
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_dpi_panel, 1,
                                                     &raw_frame_buffer);
    if (err != ESP_OK) {
        return err;
    }
    if (raw_frame_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *frame_buffer = raw_frame_buffer;

    // LCD_COLOR_FMT_RGB888 is ESP_COLOR_FOURCC_BGR24: byte order is B, G, R.
    // White, yellow, cyan, green, magenta, red, blue, black.
    static const uint8_t colors[8][3] = {
        {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
        {255, 0, 255}, {0, 0, 255}, {255, 0, 0}, {0, 0, 0},
    };
    for (uint32_t y = 0; y < WX101_HEIGHT; ++y) {
        uint8_t *row = frame_buffer + (size_t)y * WX101_WIDTH * WX101_BYTES_PER_PIXEL;
        for (uint32_t x = 0; x < WX101_WIDTH; ++x) {
            const uint8_t *color = colors[x * 8U / WX101_WIDTH];
#if CONFIG_SCREEN_ENABLE_BENCHMARK
            // Native little-endian RGB565; P4 v3 bridge expands it to RGB888.
            ((uint16_t *)row)[x] = ((uint16_t)(color[2] >> 3) << 11) |
                                  ((uint16_t)(color[1] >> 2) << 5) |
                                  (color[0] >> 3);
#else
            row[x * 3U] = color[0];
            row[x * 3U + 1U] = color[1];
            row[x * 3U + 2U] = color[2];
#endif
        }
    }

    return esp_cache_msync(frame_buffer, WX101_FRAME_BYTES,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t send_vendor_init(void)
{
    const size_t command_count = sizeof(wx101ch021i_40z_vendor_init) /
                                 sizeof(wx101ch021i_40z_vendor_init[0]);
    ESP_LOGI(TAG, "Sending %u panel initialization commands", (unsigned)command_count);
    for (size_t index = 0; index < command_count; ++index) {
        const wx101_panel_init_cmd_t *command = &wx101ch021i_40z_vendor_init[index];
        ESP_LOGD(TAG, "DCS command %u/%u: 0x%02x, %u parameter bytes",
                 (unsigned)(index + 1U), (unsigned)command_count,
                 (unsigned)command->cmd, (unsigned)command->data_bytes);
        esp_err_t err = send_command(command->cmd, command->data, command->data_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DCS command %u (0x%02x) failed: %s",
                     (unsigned)(index + 1U), (unsigned)command->cmd, esp_err_to_name(err));
            return err;
        }
        if (command->delay_ms != 0U) {
            vTaskDelay(pdMS_TO_TICKS(command->delay_ms));
        }
    }
    return ESP_OK;
}

esp_err_t wx101_panel_start(void)
{
    if (s_phy_ldo != NULL || s_dsi_bus != NULL ||
        s_command_io != NULL || s_dpi_panel != NULL) {
        ESP_LOGE(TAG, "Display resources are already held");
        return ESP_ERR_INVALID_STATE;
    }

    s_refresh_count = 0;
    const char *step = "Acquire DSI PHY LDO3 at 2500 mV";
    ESP_LOGI(TAG, "%s", step);
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_config, &s_phy_ldo);
    if (err != ESP_OK) {
        goto fail;
    }

    step = "Create 2-lane DSI bus at 1000 Mbps/lane";
    ESP_LOGI(TAG, "%s", step);
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = 1000,
        .flags.clock_lane_force_hs = false,
    };
    err = esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);
    if (err != ESP_OK) {
        goto fail;
    }

    step = "Create 8-bit DCS command IO";
    ESP_LOGI(TAG, "%s", step);
    const esp_lcd_dbi_io_config_t io_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(s_dsi_bus, &io_config, &s_command_io);
    if (err != ESP_OK) {
        goto fail;
    }
    mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(0), WX101_REQUEST_COMMAND_ACK);
    ESP_LOGI(TAG, "DCS command ACK requests: %s",
             WX101_REQUEST_COMMAND_ACK ? "enabled" : "disabled");

    step = "Create 800x1280 DPI panel, RGB888 output, 80 MHz request";
    ESP_LOGI(TAG, "%s", step);
    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 80,
        .in_color_format = WX101_INPUT_FORMAT,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = WX101_FB_COUNT,
        .video_timing = {
            .h_size = WX101_WIDTH,
            .v_size = WX101_HEIGHT,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 28,
            .vsync_front_porch = 140,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 20,
        },
        .flags.disable_lp = false,
    };
    err = esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_config, &s_dpi_panel);
    if (err != ESP_OK) {
        goto fail;
    }

#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    step = "Prepare three scanout framebuffers and ownership queues";
    err = esp_lcd_dpi_panel_get_frame_buffer(s_dpi_panel, 3,
                                            &s_framebuffers[0], &s_framebuffers[1], &s_framebuffers[2]);
    if (err != ESP_OK) goto fail;
    s_free_frames = xQueueCreate(WX101_FB_COUNT, sizeof(uint8_t));
    s_vsync = xSemaphoreCreateBinary();
    s_committed = xSemaphoreCreateBinary();
    s_present_lock = xSemaphoreCreateMutex();
    if (!s_free_frames || !s_vsync || !s_committed || !s_present_lock) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_front_index = 0;
    s_pending_index = -1;
    s_present_count = 0;
    s_last_vsync_us = 0;
    for (uint8_t index = 0; index < WX101_FB_COUNT; ++index) {
        s_fb_states[index] = index == 0 ? FB_FRONT : FB_FREE;
        if (index != 0) xQueueSend(s_free_frames, &index, 0);
    }
    ESP_LOGI(TAG, "USB scanout: 3 RGB888/BGR24 buffers, %u bytes each", (unsigned)WX101_FRAME_BYTES);
#endif

    step = "Fill static color bars and write back frame buffer cache";
    ESP_LOGI(TAG, "%s", step);
    err = fill_color_bars();
    if (err != ESP_OK) {
        goto fail;
    }

    step = "Register bridge refresh counter";
    ESP_LOGI(TAG, "%s", step);
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = on_refresh_done,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(s_dpi_panel, &callbacks, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    // No confirmed physical reset pin: use standard DCS SWRESET instead.
    step = "Send DCS software reset and wait 150 ms";
    ESP_LOGI(TAG, "%s", step);
    err = send_command(0x01, NULL, 0);
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    step = "Send exact-model vendor initialization";
    err = send_vendor_init();
    if (err != ESP_OK) {
        goto fail;
    }

    step = "Start DPI video stream";
    ESP_LOGI(TAG, "%s", step);
    err = esp_lcd_panel_init(s_dpi_panel);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "Static color bars started; refresh IRQs measure host activity only");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "%s failed: %s", step, esp_err_to_name(err));
    release_resources();
    return err;
}

esp_lcd_panel_handle_t wx101_panel_handle(void)
{
    return s_dpi_panel;
}

uint32_t wx101_panel_refresh_count(void)
{
    return s_refresh_count;
}

uint32_t wx101_panel_present_count(void)
{
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    return s_present_count;
#else
    return 0;
#endif
}

esp_err_t wx101_panel_acquire_frame(void **buffer, size_t *bytes, TickType_t timeout)
{
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    if (!buffer || !bytes || !s_free_frames) return ESP_ERR_INVALID_STATE;
    uint8_t index;
    if (xQueueReceive(s_free_frames, &index, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_frame_lock);
    bool valid = s_fb_states[index] == FB_FREE;
    if (valid) s_fb_states[index] = FB_ACQUIRED;
    portEXIT_CRITICAL(&s_frame_lock);
    if (!valid) return ESP_ERR_INVALID_STATE;
    *buffer = s_framebuffers[index];
    *bytes = WX101_FRAME_BYTES;
    return ESP_OK;
#else
    (void)buffer; (void)bytes; (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wx101_panel_release_frame(void *buffer)
{
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    for (uint8_t index = 0; index < WX101_FB_COUNT; ++index) {
        if (s_framebuffers[index] != buffer) continue;
        portENTER_CRITICAL(&s_frame_lock);
        bool valid = s_fb_states[index] == FB_ACQUIRED;
        if (valid) s_fb_states[index] = FB_FREE;
        portEXIT_CRITICAL(&s_frame_lock);
        if (!valid) return ESP_ERR_INVALID_STATE;
        return xQueueSend(s_free_frames, &index, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_INVALID_ARG;
#else
    (void)buffer;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wx101_panel_present_frame(void *buffer, uint32_t *wait_us)
{
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    if (!s_present_lock || !wait_us) return ESP_ERR_INVALID_STATE;
    int index = -1;
    for (int i = 0; i < WX101_FB_COUNT; ++i) if (s_framebuffers[i] == buffer) index = i;
    if (index < 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_present_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int64_t started = esp_timer_get_time();
    portENTER_CRITICAL(&s_frame_lock);
    bool valid = s_fb_states[index] == FB_ACQUIRED && s_pending_index < 0;
    int64_t phase_us = started - s_last_vsync_us;
    portEXIT_CRITICAL(&s_frame_lock);
    esp_err_t err = ESP_OK;
    if (!valid) { err = ESP_ERR_INVALID_STATE; goto done; }

    // The IDF DMA callback selects the requested buffer near the end of active
    // video. Publish just after VSYNC, well before the next DMA selection.
    // A presenter awakened by the last commit can use that same early window.
    if (phase_us < 0 || phase_us > 1000) {
        while (xSemaphoreTake(s_vsync, 0) == pdTRUE) {}
        if (xSemaphoreTake(s_vsync, pdMS_TO_TICKS(100)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
    }
    while (xSemaphoreTake(s_committed, 0) == pdTRUE) {}
    err = esp_lcd_panel_draw_bitmap(s_dpi_panel, 0, 0, WX101_WIDTH, WX101_HEIGHT, buffer);
    if (err != ESP_OK) goto done;
    portENTER_CRITICAL(&s_frame_lock);
    // If delayed into the late frame window, retain the old buffer for one
    // extra boundary. This prevents an uncertain DMA handoff from recycling it.
    int64_t age = esp_timer_get_time() - s_last_vsync_us;
    s_pending_due = s_refresh_count + (age < 10000 ? 1U : 2U);
    s_pending_index = index;
    s_fb_states[index] = FB_PENDING;
    portEXIT_CRITICAL(&s_frame_lock);
    if (xSemaphoreTake(s_committed, pdMS_TO_TICKS(100)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
        // A pending frame remains owned by scanout until the ISR commits it.
    }
done:
    *wait_us = (uint32_t)(esp_timer_get_time() - started);
    xSemaphoreGive(s_present_lock);
    return err;
#else
    (void)buffer; (void)wait_us;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
