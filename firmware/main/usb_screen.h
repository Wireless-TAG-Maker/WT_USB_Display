#pragma once

#include "esp_err.h"

/**
 * Start the HUSB high-speed vendor JPEG receiver after panel/JPEG startup.
 * Retains its tasks, queues and buffers until reset. Call once from task context.
 * FUSB remains the independent USB Serial/JTAG console.
 */
esp_err_t usb_screen_start(void);
