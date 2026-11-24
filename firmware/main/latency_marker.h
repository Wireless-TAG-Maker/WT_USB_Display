#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Create the GPIO51 WS2812 and LVGL markers from the LVGL calling task. */
esp_err_t latency_marker_start(void);

/** Flash LED/LCD together every 500 ms for 20 edges, then return to touch mode. */
void latency_marker_begin_flash(void);

/** In touch mode, light the LED while a pressed native-Y coordinate is >=640. */
void latency_marker_point(bool pressed, uint16_t y);

/** Call before clearing the screen: stop the timer and disable this module until reset. */
void latency_marker_suspend(void);
