#include <inttypes.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_SCREEN_ENABLE_DISPLAY
#include "wx101_panel.h"
#endif
#if CONFIG_SCREEN_ENABLE_BENCHMARK
#include "screen_benchmark.h"
#endif
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
#include "usb_screen.h"
#include "usb_jpeg.h"
#endif

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(6, 0, 2)
#error "This diagnostic firmware requires ESP-IDF 6.0.2."
#endif

#if !CONFIG_IDF_TARGET_ESP32P4
#error "This diagnostic firmware targets ESP32-P4."
#endif

#define SCREEN_I2C_SDA_GPIO GPIO_NUM_7
#define SCREEN_I2C_SCL_GPIO GPIO_NUM_8
#define SCREEN_I2C_FIRST_ADDRESS 0x08
#define SCREEN_I2C_LAST_ADDRESS 0x77
#define SCREEN_I2C_PROBE_TIMEOUT_MS 50

static const char *TAG = "screen_probe";

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

#if CONFIG_SCREEN_ENABLE_DISPLAY
    ESP_LOGI(TAG, "Profile: WT9932P4C61-TINY; WX101CH021I-40Z display trial");
#else
    ESP_LOGI(TAG, "Profile: WT9932P4C61-TINY; diagnostic mode only");
#endif
    ESP_LOGI(TAG, "IDF: %s; target: %s; chip revision: v%u.%u; cores: %u",
             esp_get_idf_version(), CONFIG_IDF_TARGET,
             (unsigned)(chip_info.revision / 100),
             (unsigned)(chip_info.revision % 100),
             (unsigned)chip_info.cores);
    ESP_LOGI(TAG, "Configured flash size: %s", CONFIG_ESPTOOLPY_FLASHSIZE);
    if (chip_info.revision != 302) {
        ESP_LOGW(TAG, "Expected the previously identified chip revision v3.2");
    }
}

static void scan_screen_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SCREEN_I2C_SDA_GPIO,
        .scl_io_num = SCREEN_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The board schematic provides 4.7 kOhm pull-ups to 3.3 V.
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(err));
        return;
    }

    unsigned found = 0;
    unsigned absent = 0;
    unsigned errors = 0;
    const int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Scanning 7-bit addresses 0x08..0x77 on SDA=7, SCL=8");
    ESP_LOGI(TAG, "Address probes only; no register reads or writes");

    for (uint16_t address = SCREEN_I2C_FIRST_ADDRESS;
         address <= SCREEN_I2C_LAST_ADDRESS; ++address) {
        // ESP-IDF probes at 100 kHz using address + write bit, with no payload.
        err = i2c_master_probe(bus, address, SCREEN_I2C_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ++found;
            ESP_LOGI(TAG, "ACK at 7-bit address 0x%02x", (unsigned)address);
        } else if (err == ESP_ERR_NOT_FOUND) {
            ++absent;
        } else {
            ++errors;
            ESP_LOGW(TAG, "Probe 0x%02x failed: %s",
                     (unsigned)address, esp_err_to_name(err));
        }
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "Scan complete: found=%u, absent=%u, errors=%u, elapsed=%" PRId64 " ms",
             found, absent, errors, (esp_timer_get_time() - started_us) / 1000);
    err = i2c_del_master_bus(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus release failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "I2C address scan finished");
}

void app_main(void)
{
    // Leave time for the USB Serial/JTAG monitor to attach after reset.
    vTaskDelay(pdMS_TO_TICKS(2000));
    log_chip_info();
    scan_screen_i2c();
#if CONFIG_SCREEN_ENABLE_DISPLAY
    esp_err_t err = wx101_panel_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display trial stopped: %s", esp_err_to_name(err));
        return;
    }
#if CONFIG_SCREEN_ENABLE_BENCHMARK
    err = screen_benchmark_run();
    ESP_LOGE(TAG, "Benchmark stopped unexpectedly: %s", esp_err_to_name(err));
    return;
#endif
#if CONFIG_SCREEN_ENABLE_USB_DISPLAY
    err = usb_jpeg_init();
    if (err == ESP_OK) err = usb_screen_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "USB display startup failed: %s", esp_err_to_name(err));
    return;
#endif
    ESP_LOGI(TAG, "Color-bar trial started; visual confirmation is required");
    uint32_t previous_count = wx101_panel_refresh_count();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t count = wx101_panel_refresh_count();
        ESP_LOGI(TAG, "DPI host refresh events in 5 seconds: %" PRIu32,
                 count - previous_count);
        previous_count = count;
    }
#endif
}
