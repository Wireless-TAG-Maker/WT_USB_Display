#pragma once

#include "esp_err.h"

/** Run LVGL from the calling task; retains buffers/timer until reset. */
esp_err_t screen_benchmark_run(void);
