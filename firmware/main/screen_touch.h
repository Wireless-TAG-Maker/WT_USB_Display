#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_TOUCH_I2C_ADDRESS 0x68
#define SCREEN_TOUCH_EXPECTED_CHIP_ID 0x9032
#define SCREEN_TOUCH_RAW_BYTES 60

/** Raw data only: no coordinate mapping, touch-count or checksum interpretation. */
typedef struct {
    uint8_t data[SCREEN_TOUCH_RAW_BYTES];
    int64_t timestamp_us; /**< Time at which this successful read completed. */
    uint32_t sequence;    /**< Successful reads since start; wraps naturally. */
} screen_touch_packet_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} screen_touch_point_t;

/**
 * Create I2C0 on SDA7/SCL8 at 100 kHz and require chip ID 0x9032 at address 0x68.
 * Sends register-address selectors only; never writes BackDoor/control values.
 * Owns the bus/device until stop. Call all functions serially from task context.
 */
esp_err_t screen_touch_start(void);

/** Change only the host I2C clock (100/400 kHz); no controller-value writes. */
esp_err_t screen_touch_set_speed(uint32_t speed_hz);

/**
 * Select 0x20021120 using a 4-byte write with STOP, then read a 60-byte packet.
 * The controller's ready section table reports this address and length.
 * Each transaction has a 100 ms timeout. Clears the output on failure.
 * All-FF reports are rejected; all-zero and repeated reports are not errors.
 * Successful I2C transport does not establish that the report format is valid.
 * Reference drivers enter BackDoor first; this module intentionally does not.
 */
esp_err_t screen_touch_read(screen_touch_packet_t *packet);

/** Read the first finger as native 800x1280 coordinates; no scaling/rotation.
 * count=0 releases; count>10 or coordinates outside the panel are rejected.
 * Decoding is verified by this panel's four-corner and drag captures. */
esp_err_t screen_touch_read_point(screen_touch_point_t *point);

/** Copy the latest successful packet bytes; raw_size must equal 60.
 * Returns ESP_ERR_NOT_FOUND before a valid read or after a read failure. */
esp_err_t screen_touch_get_raw(uint8_t *raw, size_t raw_size);

/** Invalidate cached data and release the device before releasing its bus.
 * A failed release retains the affected handles so stop can be retried. */
esp_err_t screen_touch_stop(void);

/**
 * Describe candidate SRAM coordinate-section records using read-only access.
 * Requires a started, identified controller and task-context serial access.
 * Logs raw ready/header and pointer/length bytes; does not change or follow the
 * current report selector. A non-ready header is diagnostic, not an I/O error.
 */
esp_err_t screen_touch_describe_coordinate_section(void);

#ifdef __cplusplus
}
#endif
