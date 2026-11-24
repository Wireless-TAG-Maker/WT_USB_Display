#include "screen_touch.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#define SCREEN_TOUCH_TIMEOUT_MS 100

static const char *TAG = "screen_touch";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static bool s_started;
static bool s_packet_valid;
static uint32_t s_sequence;
static uint32_t s_consecutive_errors;
static screen_touch_packet_t s_last_packet;

/* Wire selectors from the OSPTEK JD9366 reference; not register-value writes.
 * https://github.com/osptek/display-tft-10.1inch-800x1280/blob/
 * e36cd83c5d61435ad4286f70caf797a4f0996d80/examples/
 * esp32p4-idf6-jd9366-mipi-lvgl-port-lvgl9/components/
 * esp_lcd_touch_jd9366/esp_lcd_touch_jd9366.c
 * The ID selector matches rx_param(0x40008076) with 32-bit LCD command width.
 * The report selector matches tx_param(-1, {20,02,11,20}, 4), followed by STOP
 * and rx_param(-1, ..., 80). This panel's section table instead reports 60
 * bytes, so reads are bounded to that verified length. No control-value writes
 * to 0x40008004 or 0x40008081 occur here.
 */
static const uint8_t s_id_selector[4] = {0x40, 0x00, 0x80, 0x76};
static const uint8_t s_report_selector[4] = {0x20, 0x02, 0x11, 0x20};

static esp_err_t record_read_failure(const char *operation, esp_err_t err)
{
    s_packet_valid = false;
    memset(&s_last_packet, 0, sizeof(s_last_packet));
    ++s_consecutive_errors;
    if (s_consecutive_errors == 1 || s_consecutive_errors % 32 == 0) {
        ESP_LOGW(TAG, "%s failed: %s; consecutive errors=%" PRIu32,
                 operation, esp_err_to_name(err), s_consecutive_errors);
    }
    return err;
}

esp_err_t screen_touch_stop(void)
{
    s_started = false;
    s_packet_valid = false;
    memset(&s_last_packet, 0, sizeof(s_last_packet));

    if (s_device != NULL) {
        esp_err_t err = i2c_master_bus_rm_device(s_device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release touch I2C device failed: %s", esp_err_to_name(err));
            return err;
        }
        s_device = NULL;
    }
    if (s_bus != NULL) {
        esp_err_t err = i2c_del_master_bus(s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Release touch I2C bus failed: %s", esp_err_to_name(err));
            return err;
        }
        s_bus = NULL;
    }
    return ESP_OK;
}

esp_err_t screen_touch_start(void)
{
    if (s_bus != NULL || s_device != NULL || s_started) {
        ESP_LOGE(TAG, "Touch I2C resources are already held");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_7,
        .scl_io_num = GPIO_NUM_8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The WT board has external 4.7 kOhm pull-ups to 3.3 V.
        .flags.enable_internal_pullup = false,
    };
    ESP_LOGI(TAG, "Create read-only touch probe: SDA7/SCL8, 100 kHz, address 0x68");
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Create touch I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCREEN_TOUCH_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Add touch I2C device failed: %s", esp_err_to_name(err));
        goto fail;
    }

    uint8_t id_bytes[2] = {0};
    // A 4-byte address selection plus repeated START/read, with no value payload.
    err = i2c_master_transmit_receive(s_device, s_id_selector, sizeof(s_id_selector),
                                     id_bytes, sizeof(id_bytes), SCREEN_TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read ID at 0x40008076 failed: %s", esp_err_to_name(err));
        goto fail;
    }
    const uint16_t chip_id = (uint16_t)id_bytes[0] | ((uint16_t)id_bytes[1] << 8);
    ESP_LOGI(TAG, "ID raw=%02x %02x, decoded=0x%04x, expected=0x%04x",
             (unsigned)id_bytes[0], (unsigned)id_bytes[1],
             (unsigned)chip_id, (unsigned)SCREEN_TOUCH_EXPECTED_CHIP_ID);
    if (chip_id != SCREEN_TOUCH_EXPECTED_CHIP_ID) {
        ESP_LOGE(TAG, "Unexpected chip ID; touch probe stopped");
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    s_sequence = 0;
    s_consecutive_errors = 0;
    s_packet_valid = false;
    s_started = true;
    ESP_LOGI(TAG, "Chip ID accepted; BackDoor, reset and INT controls remain unused");
    return ESP_OK;

fail:
    // Preserve the original identification error even if cleanup also fails.
    (void)screen_touch_stop();
    return err;
}

esp_err_t screen_touch_read(screen_touch_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(packet, 0, sizeof(*packet));
    if (!s_started || s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_packet_valid = false;

    screen_touch_packet_t next_packet = {0};
    // Preserve OSPTEK's selector + STOP/read; use the panel's verified 60B length.
    // The selector is not a touch-reset/BackDoor/control-register value write.
    esp_err_t err = i2c_master_transmit(s_device, s_report_selector,
                                       sizeof(s_report_selector), SCREEN_TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) {
        return record_read_failure("Select touch report address", err);
    }
    err = i2c_master_receive(s_device, next_packet.data, sizeof(next_packet.data),
                             SCREEN_TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) {
        return record_read_failure("Read 60-byte touch report", err);
    }

    bool all_ff = true;
    for (size_t index = 0; index < sizeof(next_packet.data); ++index) {
        if (next_packet.data[index] != 0xff) {
            all_ff = false;
            break;
        }
    }
    if (all_ff) {
        return record_read_failure("All-FF touch report", ESP_ERR_INVALID_RESPONSE);
    }

    // All-zero can be a released state. Identical packets can be a held touch.
    // No undocumented header, checksum or coordinate assumptions are made here.
    next_packet.timestamp_us = esp_timer_get_time();
    next_packet.sequence = ++s_sequence;
    if (s_consecutive_errors != 0) {
        ESP_LOGI(TAG, "Touch report transport recovered after %" PRIu32 " errors",
                 s_consecutive_errors);
    }
    s_consecutive_errors = 0;
    s_last_packet = next_packet;
    s_packet_valid = true;
    *packet = next_packet;
    return ESP_OK;
}

esp_err_t screen_touch_get_raw(uint8_t *raw, size_t raw_size)
{
    if (raw == NULL || raw_size != SCREEN_TOUCH_RAW_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(raw, 0, raw_size);
    if (!s_started || !s_packet_valid) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(raw, s_last_packet.data, raw_size);
    return ESP_OK;
}

esp_err_t screen_touch_read_point(screen_touch_point_t *point)
{
    if (point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(point, 0, sizeof(*point));
    screen_touch_packet_t packet;
    esp_err_t err = screen_touch_read(&packet);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t count = packet.data[0];
    if (count > 10) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (count == 0) {
        return ESP_OK;
    }
    // First point starts at byte 3, with big-endian 16-bit X and Y.
    // Real corner captures include (1,0), (4,1279), (797,1), (797,1279).
    // Multiplication by 255 or resolution-dependent scaling is incorrect here.
    uint16_t x = ((uint16_t)packet.data[3] << 8) | packet.data[4];
    uint16_t y = ((uint16_t)packet.data[5] << 8) | packet.data[6];
    if (x >= 800 || y >= 1280) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    point->x = x;
    point->y = y;
    point->pressed = true;
    return ESP_OK;
}

esp_err_t screen_touch_set_speed(uint32_t speed_hz)
{
    if (!s_started || s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speed_hz != 100000 && speed_hz != 400000) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    if (s_device != NULL) {
        err = i2c_master_bus_rm_device(s_device);
        if (err != ESP_OK) {
            return err;
        }
        s_device = NULL;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCREEN_TOUCH_I2C_ADDRESS,
        .scl_speed_hz = speed_hz,
    };
    err = i2c_master_bus_add_device(s_bus, &config, &s_device);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t id[2];
    err = i2c_master_transmit_receive(s_device, s_id_selector, sizeof(s_id_selector),
                                     id, sizeof(id), SCREEN_TOUCH_TIMEOUT_MS);
    if (err == ESP_OK && ((uint16_t)id[0] | ((uint16_t)id[1] << 8)) != SCREEN_TOUCH_EXPECTED_CHIP_ID) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

esp_err_t screen_touch_describe_coordinate_section(void)
{
    if (!s_started || s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Layout only, from the fixed VIEWE Linux proposal's
     * jd_section_ready()/jd_resolve_coord_addr():
     * https://github.com/HF-2001/linux/blob/
     * 1932bd4b288d671b244acf759726547211fc484f/drivers/input/touchscreen/jadard_jd9365tx.c
     * ESRAM count = BASE + 4 + 25 * 8; section array = count + 4;
     * coordinate entry (index 1) = array + 1 * 8 = BASE + 0xd8.
     * The proposal targets another panel; neither candidate is assumed valid.
     * Keep this board's proven direct four-byte selector protocol, not F3 or
     * BackDoor mode. Never dereference the coordinate pointer or alter
     * s_report_selector. The separate FW-info probe below follows only its
     * validated RAM pointer, as described by the exact 0x9032 vendor source.
     */
    static const uint32_t candidate_bases[] = {0x20021000U, 0x20011000U};
    const uint32_t coordinate_entry_offset = 4U + 25U * 8U + 4U + 1U * 8U;
    esp_err_t first_error = ESP_OK;

    for (size_t candidate = 0;
         candidate < sizeof(candidate_bases) / sizeof(candidate_bases[0]);
         ++candidate) {
        const uint32_t base = candidate_bases[candidate];
        uint8_t selector[4] = {
            (uint8_t)(base >> 24), (uint8_t)(base >> 16),
            (uint8_t)(base >> 8), (uint8_t)base,
        };
        uint8_t ready_bytes[2] = {0};
        esp_err_t err = i2c_master_transmit_receive(s_device, selector, sizeof(selector),
                                                   ready_bytes, sizeof(ready_bytes),
                                                   SCREEN_TOUCH_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Section candidate base=0x%08" PRIx32 " header read failed: %s",
                     base, esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
            continue;
        }
        const uint16_t ready = (uint16_t)ready_bytes[0] | ((uint16_t)ready_bytes[1] << 8);
        ESP_LOGI(TAG, "Section candidate base=0x%08" PRIx32
                 " header_raw=%02x %02x ready_le=0x%04x",
                 base, (unsigned)ready_bytes[0], (unsigned)ready_bytes[1], (unsigned)ready);
        if (ready != 0xa55aU) {
            ESP_LOGI(TAG, "Section candidate not ready; no table/pointer read attempted");
            continue;
        }

        if (base == 0x20021000U) {
            /* Exact JD9366T_ID=0x9032 reference:
             * coolpi-george/coolpi-kernel @ 7a12feb8dddeb7fb2c5c0b3df67a861bc666d4cd,
             * jadard_ic_JD9366T.c:1365 and module_jd9366t_read_fw_ver():3151.
             * DSRAM index 0 is FW_CFG_INFO; its entry starts at BASE + 4.
             * Its data is CID BE32 +0, maker +4, panel +5, FW BE24 +7.
             * Read only that ten-byte prefix, never POR or controller values.
             */
            const uint32_t fw_entry_address = base + 4U;
            selector[0] = (uint8_t)(fw_entry_address >> 24);
            selector[1] = (uint8_t)(fw_entry_address >> 16);
            selector[2] = (uint8_t)(fw_entry_address >> 8);
            selector[3] = (uint8_t)fw_entry_address;
            uint8_t fw_entry[8] = {0};
            err = i2c_master_transmit_receive(s_device, selector, sizeof(selector),
                                             fw_entry, sizeof(fw_entry), SCREEN_TOUCH_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "FW section entry=0x%08" PRIx32 " read failed: %s",
                         fw_entry_address, esp_err_to_name(err));
                if (first_error == ESP_OK) first_error = err;
            } else {
                const uint32_t fw_pointer = (uint32_t)fw_entry[0] | ((uint32_t)fw_entry[1] << 8) |
                                            ((uint32_t)fw_entry[2] << 16) | ((uint32_t)fw_entry[3] << 24);
                const uint32_t fw_length = (uint32_t)fw_entry[4] | ((uint32_t)fw_entry[5] << 8) |
                                           ((uint32_t)fw_entry[6] << 16) | ((uint32_t)fw_entry[7] << 24);
                ESP_LOGI(TAG, "FW section entry=0x%08" PRIx32
                         " raw=%02x %02x %02x %02x %02x %02x %02x %02x"
                         " pointer_le=0x%08" PRIx32 " length_le=%" PRIu32,
                         fw_entry_address, (unsigned)fw_entry[0], (unsigned)fw_entry[1],
                         (unsigned)fw_entry[2], (unsigned)fw_entry[3], (unsigned)fw_entry[4],
                         (unsigned)fw_entry[5], (unsigned)fw_entry[6], (unsigned)fw_entry[7],
                         fw_pointer, fw_length);
                uint8_t fw_info[10] = {0};
                const uint32_t ram_end_exclusive = 0x20040000U;
                if (fw_pointer < 0x20000000U ||
                    fw_pointer > ram_end_exclusive - sizeof(fw_info) ||
                    fw_length < sizeof(fw_info)) {
                    ESP_LOGI(TAG, "FW section pointer/length outside allowed RAM prefix; data read skipped");
                } else {
                    selector[0] = (uint8_t)(fw_pointer >> 24);
                    selector[1] = (uint8_t)(fw_pointer >> 16);
                    selector[2] = (uint8_t)(fw_pointer >> 8);
                    selector[3] = (uint8_t)fw_pointer;
                    err = i2c_master_transmit_receive(s_device, selector, sizeof(selector),
                                                     fw_info, sizeof(fw_info), SCREEN_TOUCH_TIMEOUT_MS);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "FW info address=0x%08" PRIx32 " read failed: %s",
                                 fw_pointer, esp_err_to_name(err));
                        if (first_error == ESP_OK) first_error = err;
                    } else {
                        const uint32_t cid = ((uint32_t)fw_info[0] << 24) |
                                             ((uint32_t)fw_info[1] << 16) |
                                             ((uint32_t)fw_info[2] << 8) | (uint32_t)fw_info[3];
                        const uint32_t fw_version = ((uint32_t)fw_info[7] << 16) |
                                                    ((uint32_t)fw_info[8] << 8) | (uint32_t)fw_info[9];
                        ESP_LOGI(TAG, "FW info address=0x%08" PRIx32
                                 " raw=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x"
                                 " CID_BE32=0x%08" PRIx32 " panel_maker=0x%02x panel_version=0x%02x"
                                 " FW_version_BE24=0x%06" PRIx32,
                                 fw_pointer, (unsigned)fw_info[0], (unsigned)fw_info[1],
                                 (unsigned)fw_info[2], (unsigned)fw_info[3], (unsigned)fw_info[4],
                                 (unsigned)fw_info[5], (unsigned)fw_info[6], (unsigned)fw_info[7],
                                 (unsigned)fw_info[8], (unsigned)fw_info[9], cid,
                                 (unsigned)fw_info[4], (unsigned)fw_info[5], fw_version);
                    }
                }
            }
        }

        const uint32_t entry_address = base + coordinate_entry_offset;
        selector[0] = (uint8_t)(entry_address >> 24);
        selector[1] = (uint8_t)(entry_address >> 16);
        selector[2] = (uint8_t)(entry_address >> 8);
        selector[3] = (uint8_t)entry_address;
        uint8_t entry[8] = {0};
        err = i2c_master_transmit_receive(s_device, selector, sizeof(selector),
                                         entry, sizeof(entry), SCREEN_TOUCH_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Section candidate entry=0x%08" PRIx32 " read failed: %s",
                     entry_address, esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
            continue;
        }
        const uint32_t pointer = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8) |
                                 ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24);
        const uint32_t length = (uint32_t)entry[4] | ((uint32_t)entry[5] << 8) |
                                ((uint32_t)entry[6] << 16) | ((uint32_t)entry[7] << 24);
        ESP_LOGI(TAG, "Section candidate entry=0x%08" PRIx32
                 " raw=%02x %02x %02x %02x %02x %02x %02x %02x"
                 " pointer_le=0x%08" PRIx32 " length_le=%" PRIu32
                 "; diagnostic only, pointer not followed",
                 entry_address, (unsigned)entry[0], (unsigned)entry[1],
                 (unsigned)entry[2], (unsigned)entry[3], (unsigned)entry[4],
                 (unsigned)entry[5], (unsigned)entry[6], (unsigned)entry[7], pointer, length);
    }
    return first_error;
}
