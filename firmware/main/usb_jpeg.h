#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_JPEG_WIDTH 800U
#define USB_JPEG_HEIGHT 1280U
#define USB_JPEG_RGB888_BYTES ((size_t)USB_JPEG_WIDTH * USB_JPEG_HEIGHT * 3U)
#define USB_JPEG_MAX_INPUT_BYTES (2U * 1024U * 1024U)

/**
 * Acquire one ESP-IDF 6.0.2/P4 JPEG decoder until reset; repeated init is OK.
 * Task context only. This module must be the only JPEG engine user: the SDK
 * shares a codec mutex with other encoders/decoders. Concurrent calls to this
 * module are rejected. Hardware completion has a 100 ms SDK timeout.
 */
esp_err_t usb_jpeg_init(void);

/**
 * Decode one complete, 8-bit baseline, single-scan YCbCr JPEG into 800x1280
 * packed BGR24 (RGB888) pixels (stride 2400, 3072000 bytes).
 * Supports 4:4:4, 4:2:2 and 4:2:0; component IDs must be 1,2,3. Progressive,
 * gray, CMYK/RGB JPEG, multiple scans and wrong dimensions are rejected.
 * jpeg_len is the real SOI-through-EOI length, excluding USB/padding bytes,
 * and must not exceed USB_JPEG_MAX_INPUT_BYTES. Output capacity must be at
 * least USB_JPEG_RGB888_BYTES. Input and output must not overlap.
 *
 * Allocation/cache contract (SDK components/esp_driver_jpeg/jpeg_decode.c):
 * - Both buffers must reside entirely in PSRAM. Allocate input with
 *   jpeg_alloc_decoder_mem(... JPEG_DEC_ALLOC_INPUT_BUFFER ...), or equivalent
 *   CPU-readable/writable PSRAM; the input address/length need not be aligned.
 * - Allocate output with jpeg_alloc_decoder_mem(... JPEG_DEC_ALLOC_OUTPUT_BUFFER
 *   ...), and retain its allocated size. Output base must be cache-line aligned;
 *   this function passes exactly USB_JPEG_RGB888_BYTES (also cache-line aligned)
 *   to the SDK, even if capacity is larger. A native DPI PSRAM framebuffer can
 *   be used if it has this size/format and passes the same runtime alignment
 *   checks. No CPU/DMA producer may modify either buffer during this call.
 * - Input must already be CPU coherent. If another DMA engine filled it, its
 *   owner must finish/invalidate before the CPU header reads here. The JPEG
 *   driver writes input cache back and invalidates output before/after DMA.
 * - This function does not coordinate DSI scanout. Decode into an inactive
 *   framebuffer and present it after ESP_OK to avoid concurrent scan/write;
 *   a single active framebuffer may tear. On error, output must not be shown.
 *
 * decode_us may be NULL. Otherwise it is zero for preflight rejection, or the
 * elapsed jpeg_decoder_process() time (including its parsing/cache/DMA work).
 * Buffers are no longer used after return, including SDK timeout recovery.
 */
esp_err_t usb_jpeg_decode(const uint8_t *jpeg, size_t jpeg_len, void *rgb888,
                          size_t capacity, uint32_t *decode_us);

#ifdef __cplusplus
}
#endif
