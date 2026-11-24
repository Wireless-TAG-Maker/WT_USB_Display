set(app_sources "main.c")
# Requirements are expanded before Kconfig values are available.
set(app_requires esp_driver_i2c esp_system esp_timer esp_lcd esp_hw_support esp_mm esp_hal_lcd lvgl led_strip esp_tinyusb esp_driver_jpeg)
if(CONFIG_SCREEN_ENABLE_DISPLAY)
    list(APPEND app_sources "wx101_panel.c")
endif()
if(CONFIG_SCREEN_ENABLE_BENCHMARK)
    list(APPEND app_sources "screen_benchmark.c")
endif()
if(CONFIG_SCREEN_ENABLE_TOUCH_PROBE)
    list(APPEND app_sources "screen_touch.c")
endif()
if(CONFIG_SCREEN_TOUCH_LATENCY_MARKER)
    list(APPEND app_sources "latency_marker.c")
endif()
if(CONFIG_SCREEN_ENABLE_USB_DISPLAY)
    list(APPEND app_sources "usb_screen.c" "usb_jpeg.c")
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${app_requires}
)
