cmake_minimum_required(VERSION 3.16)

set(SUPPORTED_TARGETS esp32p4)
# TinyUSB vendor RX batches use the 8KiB endpoint buffer. Host frames end with
# a short packet (or explicit ZLP), while the application parses a byte stream.
add_compile_definitions(CFG_TUD_VENDOR_RX_NEED_ZLP=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wt_p4c61_screen_probe)
