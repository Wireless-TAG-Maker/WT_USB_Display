#include "usb_jpeg.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* SDK 6.0.2 sources used for the format/memory/timeout contract:
 * components/esp_driver_jpeg/include/driver/jpeg_types.h
 * components/esp_driver_jpeg/jpeg_decode.c: jpeg_decoder_process(),
 *   jpeg_alloc_decoder_mem(), jpeg_dec_config_dma_csc().
 * RGB888+BGR keeps packed B,G,R byte order, matching LCD_COLOR_FMT_RGB888.
 * RGB order would reverse those three bytes in the hardware DMA output.
 * MCU padding is irrelevant for this fixed size (both dimensions /16).
 */
_Static_assert(USB_JPEG_WIDTH % 16U == 0 && USB_JPEG_HEIGHT % 16U == 0,
               "JPEG output must not require MCU padding");

static const char *TAG = "usb_jpeg";
static jpeg_decoder_handle_t s_decoder;
static portMUX_TYPE s_call_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_in_call;

static bool claim_call(void)
{
    portENTER_CRITICAL(&s_call_lock);
    bool claimed = !s_in_call;
    if (claimed) s_in_call = true;
    portEXIT_CRITICAL(&s_call_lock);
    return claimed;
}

static void release_call(void)
{
    portENTER_CRITICAL(&s_call_lock);
    s_in_call = false;
    portEXIT_CRITICAL(&s_call_lock);
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static bool psram_range(const void *buffer, size_t size)
{
    uintptr_t start = (uintptr_t)buffer;
    return buffer != NULL && size != 0 && size <= UINTPTR_MAX - start &&
           esp_ptr_external_ram(buffer) &&
           esp_ptr_external_ram((const void *)(start + size - 1U));
}

/* Validate framing/segment lengths before invoking the SDK, including the real
 * SOF dimensions and the one interleaved baseline scan. Table payload bounds
 * are checked here; the SDK performs the remaining DQT/DHT/hardware validation.
 */
static esp_err_t preflight(const uint8_t *jpeg, size_t size,
                           jpeg_down_sampling_type_t *sampling)
{
    if (size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[size - 2] != 0xff || jpeg[size - 1] != 0xd9) {
        return ESP_ERR_INVALID_ARG;
    }

    bool have_sof = false;
    uint8_t quant_present = 0;
    uint8_t quant_required = 0;
    uint8_t huff_present = 0;
    size_t offset = 2;
    while (offset < size) {
        if (jpeg[offset++] != 0xff) return ESP_ERR_INVALID_ARG;
        while (offset < size && jpeg[offset] == 0xff) ++offset;
        if (offset >= size) return ESP_ERR_INVALID_ARG;
        const uint8_t marker = jpeg[offset++];
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 ||
            (marker >= 0xd0 && marker <= 0xd7)) return ESP_ERR_INVALID_ARG;
        if (size - offset < 2) return ESP_ERR_INVALID_ARG;
        const size_t segment_size = read_be16(jpeg + offset);
        if (segment_size < 2 || segment_size > size - offset) return ESP_ERR_INVALID_ARG;
        const uint8_t *data = jpeg + offset + 2;
        const size_t data_size = segment_size - 2;
        offset += segment_size;

        if (marker == 0xc0) {
            if (have_sof || data_size != 15) return ESP_ERR_INVALID_ARG;
            if (data[0] != 8 || data[5] != 3) return ESP_ERR_NOT_SUPPORTED;
            if (read_be16(data + 1) != USB_JPEG_HEIGHT ||
                read_be16(data + 3) != USB_JPEG_WIDTH) return ESP_ERR_INVALID_SIZE;
            for (unsigned component = 0; component < 3; ++component) {
                if (data[6 + component * 3] != component + 1 ||
                    data[8 + component * 3] > 3) return ESP_ERR_NOT_SUPPORTED;
                quant_required |= (uint8_t)(1U << data[8 + component * 3]);
            }
            if (data[10] != 0x11 || data[13] != 0x11) return ESP_ERR_NOT_SUPPORTED;
            switch (data[7]) {
            case 0x11: *sampling = JPEG_DOWN_SAMPLING_YUV444; break;
            case 0x21: *sampling = JPEG_DOWN_SAMPLING_YUV422; break;
            case 0x22: *sampling = JPEG_DOWN_SAMPLING_YUV420; break;
            default: return ESP_ERR_NOT_SUPPORTED;
            }
            have_sof = true;
        } else if (marker == 0xdb) {
            size_t table_offset = 0;
            while (table_offset < data_size) {
                uint8_t id = data[table_offset++];
                if ((id >> 4) != 0 || (id & 15) > 3) return ESP_ERR_NOT_SUPPORTED;
                if (data_size - table_offset < 64) return ESP_ERR_INVALID_ARG;
                for (unsigned index = 0; index < 64; ++index) {
                    if (data[table_offset + index] == 0) return ESP_ERR_INVALID_ARG;
                }
                quant_present |= (uint8_t)(1U << (id & 15));
                table_offset += 64;
            }
        } else if (marker == 0xc4) {
            size_t table_offset = 0;
            while (table_offset < data_size) {
                if (data_size - table_offset < 17) return ESP_ERR_INVALID_ARG;
                const uint8_t id = data[table_offset++];
                if ((id >> 4) > 1 || (id & 15) > 1) return ESP_ERR_NOT_SUPPORTED;
                size_t symbols = 0;
                for (unsigned index = 0; index < 16; ++index) symbols += data[table_offset + index];
                table_offset += 16;
                if (symbols == 0 || symbols > 256 || symbols > data_size - table_offset) {
                    return ESP_ERR_INVALID_ARG;
                }
                huff_present |= (uint8_t)(1U << ((id >> 4) * 2U + (id & 15)));
                table_offset += symbols;
            }
        } else if (marker == 0xdd) {
            if (data_size != 2) return ESP_ERR_INVALID_ARG;
        } else if (marker == 0xda) {
            if (!have_sof || data_size != 10 || data[0] != 3) return ESP_ERR_NOT_SUPPORTED;
            if (data[7] != 0 || data[8] != 63 || data[9] != 0) return ESP_ERR_NOT_SUPPORTED;
            // The SDK programs quantization slots 0..qt_tbl_num-1. Reject
            // sparse table IDs rather than decoding against an unprogrammed slot.
            if ((quant_present & (quant_present + 1U)) != 0) return ESP_ERR_NOT_SUPPORTED;
            uint8_t huff_required = 0;
            for (unsigned component = 0; component < 3; ++component) {
                const uint8_t id = data[1 + component * 2];
                const uint8_t tables = data[2 + component * 2];
                if (id != component + 1 || (tables >> 4) > 1 || (tables & 15) > 1) {
                    return ESP_ERR_NOT_SUPPORTED;
                }
                huff_required |= (uint8_t)((1U << (tables >> 4)) | (1U << (2U + (tables & 15))));
            }
            if ((quant_present & quant_required) != quant_required ||
                (huff_present & huff_required) != huff_required) return ESP_ERR_INVALID_ARG;

            const size_t entropy_start = offset;
            while (offset < size) {
                if (jpeg[offset++] != 0xff) continue;
                while (offset < size && jpeg[offset] == 0xff) ++offset;
                if (offset >= size) return ESP_ERR_INVALID_ARG;
                const uint8_t code = jpeg[offset++];
                if (code == 0 || (code >= 0xd0 && code <= 0xd7)) continue;
                if (code == 0xd9) {
                    return offset == size && offset > entropy_start + 2 ? ESP_OK : ESP_ERR_INVALID_ARG;
                }
                return ESP_ERR_NOT_SUPPORTED; // Includes progressive/multiple scans.
            }
            return ESP_ERR_INVALID_ARG;
        } else if (marker == 0xee) {
            // An Adobe RGB/CMYK transform is not the YCbCr input expected here.
            if (data_size >= 5 && memcmp(data, "Adobe", 5) == 0 &&
                (data_size < 12 || data[11] != 1)) return ESP_ERR_NOT_SUPPORTED;
        } else if (!((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t usb_jpeg_init(void)
{
    if (!claim_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    if (s_decoder == NULL) {
        const jpeg_decode_engine_cfg_t config = {
            .timeout_ms = 100,
        };
        err = jpeg_new_decoder_engine(&config, &s_decoder);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "P4 hardware JPEG decoder: 800x1280 RGB888/BGR24, timeout=100ms");
        }
    }
    release_call();
    return err;
}

esp_err_t usb_jpeg_decode(const uint8_t *jpeg, size_t jpeg_len, void *rgb888,
                          size_t capacity, uint32_t *decode_us)
{
    if (decode_us != NULL) *decode_us = 0;
    if (jpeg == NULL || rgb888 == NULL || jpeg_len == 0) return ESP_ERR_INVALID_ARG;
    if (jpeg_len > USB_JPEG_MAX_INPUT_BYTES || jpeg_len > UINT32_MAX ||
        capacity < USB_JPEG_RGB888_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!psram_range(jpeg, jpeg_len) || !psram_range(rgb888, USB_JPEG_RGB888_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }
    uintptr_t input_begin = (uintptr_t)jpeg;
    uintptr_t output_begin = (uintptr_t)rgb888;
    if (input_begin < output_begin + USB_JPEG_RGB888_BYTES &&
        output_begin < input_begin + jpeg_len) return ESP_ERR_INVALID_ARG;
    size_t alignment = esp_cache_get_line_size_by_addr(rgb888);
    if (alignment == 0) alignment = 4; // Same fallback as SDK _check_buffer_alignment().
    if (output_begin % alignment != 0 || USB_JPEG_RGB888_BYTES % alignment != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claim_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t err;
    if (s_decoder == NULL) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    jpeg_down_sampling_type_t sampling = JPEG_DOWN_SAMPLING_GRAY;
    err = preflight(jpeg, jpeg_len, &sampling);
    if (err != ESP_OK) goto done;
    jpeg_decode_picture_info_t info = {0};
    err = jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_len, &info);
    if (err != ESP_OK) goto done;
    if (info.width != USB_JPEG_WIDTH || info.height != USB_JPEG_HEIGHT ||
        info.sample_method != sampling) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t output_bytes = 0;
    int64_t start_us = esp_timer_get_time();
    err = jpeg_decoder_process(s_decoder, &config, jpeg, (uint32_t)jpeg_len,
                               rgb888, (uint32_t)USB_JPEG_RGB888_BYTES, &output_bytes);
    if (decode_us != NULL) {
        int64_t elapsed = esp_timer_get_time() - start_us;
        *decode_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    }
    if (err == ESP_OK && output_bytes != USB_JPEG_RGB888_BYTES) err = ESP_ERR_INVALID_SIZE;

done:
    release_call();
    return err;
}
