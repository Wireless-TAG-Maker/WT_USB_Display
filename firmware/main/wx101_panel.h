// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the WX101CH021I-40Z with a static color-bar frame.
 *
 * Call from task context. The module retains the DSI PHY, bus, command IO and
 * framebuffers until reset (three in USB mode, one otherwise). A second call
 * while resources are held returns ESP_ERR_INVALID_STATE. Failed initialization
 * releases created resources in reverse order where possible.
 */
esp_err_t wx101_panel_start(void);

/** Borrow the initialized native DPI panel. NULL before successful startup. */
esp_lcd_panel_handle_t wx101_panel_handle(void);

/**
 * @brief Return the bridge refresh count, wrapping naturally at UINT32_MAX.
 *
 * This counts host-side refresh interrupts and does not prove visible output.
 */
uint32_t wx101_panel_refresh_count(void);

/** USB profile only: borrow an off-screen packed BGR24 (RGB888) framebuffer. */
esp_err_t wx101_panel_acquire_frame(void **buffer, size_t *bytes, TickType_t timeout);
/** Return an acquired frame that will not be presented. */
esp_err_t wx101_panel_release_frame(void *buffer);
/** Submit a completed frame and wait for its scanout handoff (100ms bound). */
esp_err_t wx101_panel_present_frame(void *buffer, uint32_t *wait_us);
/** Distinct USB frames handed to scanout, separate from repeated refresh IRQs. */
uint32_t wx101_panel_present_count(void);

#ifdef __cplusplus
}
#endif
