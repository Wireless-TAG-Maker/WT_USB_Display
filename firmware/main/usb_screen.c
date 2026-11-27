#include "usb_screen.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <uchar.h>

#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_jpeg.h"
#include "usb_screen_protocol.h"
#include "wx101_panel.h"

#if !CONFIG_IDF_TARGET_ESP32P4
#error "This HUSB implementation is for the verified ESP32-P4 board."
#endif
#if !CONFIG_TINYUSB_MODE_DMA || CONFIG_TINYUSB_VENDOR_COUNT != 1
#error "Enable TinyUSB Buffer DMA and exactly one vendor interface."
#endif
#if CONFIG_TINYUSB_VENDOR_EPSIZE != 8192 || CONFIG_TINYUSB_VENDOR_RX_BUFSIZE < 8192 || CONFIG_TINYUSB_VENDOR_TX_BUFSIZE < 512
#error "Use vendor EPSIZE=8192, RX FIFO >=8192, TX FIFO >=512."
#endif
#if !CFG_TUD_VENDOR_RX_NEED_ZLP || !CFG_TUD_VENDOR_TXRX_BUFFERED
#error "This receiver requires buffered vendor RX and CFG_TUD_VENDOR_RX_NEED_ZLP=1."
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The shared wire structures require a little-endian target."
#endif

#define JPEG_BUFFER_COUNT 2
#define RX_CHUNK_BYTES 8192
#define FRAME_QUEUE_LENGTH 2
#define DECODED_QUEUE_LENGTH 1
#define ACK_QUEUE_LENGTH 4
#define FRAME_TIMEOUT_US 2000000LL
#define PIPELINE_TIMEOUT_US 2000000LL
#define CONFIG_DESC_BYTES (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const char *TAG = "usb_screen";
static uint8_t *s_jpeg[JPEG_BUFFER_COUNT];
static uint8_t *s_rx_chunk;
static QueueHandle_t s_free_queue, s_frame_queue, s_decoded_queue, s_ack_queue;
static TaskHandle_t s_rx_task, s_decode_task, s_present_task;
static atomic_bool s_started, s_link_up, s_hello_seen;
static atomic_uint_least32_t s_generation;

static struct {
    atomic_uint_least32_t bytes, received, decoded;
    atomic_uint_least32_t crc_errors, header_errors, decode_errors, present_errors;
    atomic_uint_least32_t dropped, timeouts, ack_timeouts, resync_bytes;
    atomic_uint_least32_t frame_queue_high, decoded_queue_high;
} s_stats;

typedef struct {
    usb_screen_frame_header_t header;
    uint32_t generation;
    uint32_t status;
    int buffer_index;
} frame_job_t;

typedef struct {
    void *framebuffer;
    uint32_t sequence, generation, decode_us;
} decoded_job_t;

typedef struct {
    usb_screen_ack_t wire;
    uint32_t generation;
} ack_job_t;

typedef struct {
    uint8_t header_bytes[sizeof(usb_screen_frame_header_t)];
    size_t header_used, payload_used;
    usb_screen_frame_header_t header;
    uint32_t generation, crc;
    int buffer_index;
    bool header_valid, job_pending;
    frame_job_t pending_job;
    int64_t last_data_us;
} parser_t;


/* Microsoft OS 2.0 device-level descriptors for the single HUSB interface. */
enum { MS_OS_REQUEST = 0x21 };
typedef struct __attribute__((packed)) {
    uint16_t length, type, data_type, name_bytes;
    char16_t name[21];
    uint16_t data_bytes;
    char16_t value[40];
} winusb_property_t;

typedef struct __attribute__((packed)) {
    uint16_t header_bytes, header_type;
    uint32_t windows_version;
    uint16_t total_bytes;
    uint16_t id_bytes, id_type;
    char compatible_id[8], sub_compatible_id[8];
    winusb_property_t property;
} winusb_set_t;

static const winusb_set_t s_winusb_set = {
    .header_bytes = 10, .header_type = MS_OS_20_SET_HEADER_DESCRIPTOR,
    .windows_version = 0x06030000, .total_bytes = sizeof(winusb_set_t),
    .id_bytes = 20, .id_type = MS_OS_20_FEATURE_COMPATBLE_ID,
    .compatible_id = "WINUSB",
    .property = {
        .length = sizeof(winusb_property_t), .type = MS_OS_20_FEATURE_REG_PROPERTY,
        .data_type = 7, /* REG_MULTI_SZ */
        .name_bytes = sizeof(u"DeviceInterfaceGUIDs"), .name = u"DeviceInterfaceGUIDs",
        .data_bytes = sizeof(u"{BFC14F27-3D79-4633-AC57-6852FF6299C4}\0"),
        .value = u"{BFC14F27-3D79-4633-AC57-6852FF6299C4}\0",
    },
};
_Static_assert(sizeof(winusb_set_t) == 162, "MS OS descriptor set size");

static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(sizeof(s_winusb_set), MS_OS_REQUEST),
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return s_bos;
}

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = USB_SCREEN_VID,
    .idProduct = USB_SCREEN_PID,
    .bcdDevice = 0x0101,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t s_fs_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_DESC_BYTES, 0, 500),
    TUD_VENDOR_DESCRIPTOR(0, 4, USB_SCREEN_EP_OUT, USB_SCREEN_EP_IN, 64),
};
static const uint8_t s_hs_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_DESC_BYTES, 0, 500),
    TUD_VENDOR_DESCRIPTOR(0, 4, USB_SCREEN_EP_OUT, USB_SCREEN_EP_IN, 512),
};
static const char *s_strings[] = {
    (const char[]){0x09, 0x04},
    "WT development",
    "WT P4 WX101 USB screen (test)",
    "WT-P4-WX101-001",
    "WP4D JPEG display",
};
static const usb_screen_hello_t s_hello = {
    .magic = USB_SCREEN_HELLO_MAGIC,
    .version = USB_SCREEN_VERSION,
    .header_bytes = sizeof(usb_screen_hello_t),
    .width = USB_SCREEN_WIDTH,
    .height = USB_SCREEN_HEIGHT,
    .max_jpeg_bytes = USB_SCREEN_MAX_JPEG,
    .formats = 1U << USB_SCREEN_FORMAT_JPEG,
    .preferred_refresh_millihz = 60000,
    .capabilities = 3, // bit0: hardware JPEG; bit1: multiple scanout framebuffers.
};

static bool session_live(uint32_t generation)
{
    return atomic_load(&s_link_up) && atomic_load(&s_hello_seen) &&
           generation == atomic_load(&s_generation);
}

static void notify_receiver(void)
{
    if (s_rx_task != NULL) xTaskNotifyGive(s_rx_task);
}

static void queue_high_water(QueueHandle_t queue, atomic_uint_least32_t *high)
{
    uint_least32_t value = uxQueueMessagesWaiting(queue);
    // A higher-priority consumer may already have removed the successful send
    // before this sample. At least one item existed at the enqueue boundary.
    if (value == 0) value = 1;
    uint_least32_t old = atomic_load(high);
    while (value > old && !atomic_compare_exchange_weak(high, &old, value)) {}
}

static void return_jpeg(int index)
{
    // Each index has exactly one owner: free queue, RX parser, or frame worker.
    configASSERT(index >= 0 && index < JPEG_BUFFER_COUNT);
    BaseType_t sent = xQueueSend(s_free_queue, &index, 0);
    configASSERT(sent == pdTRUE);
    notify_receiver();
}

static void invalidate_session(void)
{
    // A stalled host must HELLO again. Never overwrite a busy frame/ACK buffer.
    atomic_store(&s_hello_seen, false);
    atomic_fetch_add(&s_generation, 1);
    notify_receiver();
}

static ack_job_t make_ack(uint32_t sequence, uint32_t generation, uint32_t status,
                          uint32_t decode_us, uint32_t present_us)
{
    return (ack_job_t){
        .generation = generation,
        .wire = {
            .magic = USB_SCREEN_ACK_MAGIC,
            .version = USB_SCREEN_VERSION,
            .header_bytes = sizeof(usb_screen_ack_t),
            .sequence = sequence,
            .status = status,
            // Received counts complete payloads, including ones with bad CRC.
            .received_frames = atomic_load(&s_stats.received),
            .decoded_frames = atomic_load(&s_stats.decoded),
            .presented_frames = wx101_panel_present_count(),
            .decode_us = decode_us,
            .present_us = present_us,
            .host_refresh_events = wx101_panel_refresh_count(),
        },
    };
}

static void post_ack(ack_job_t *ack)
{
    int64_t deadline = esp_timer_get_time() + PIPELINE_TIMEOUT_US;
    while (session_live(ack->generation)) {
        if (xQueueSend(s_ack_queue, ack, pdMS_TO_TICKS(20)) == pdTRUE) {
            notify_receiver();
            return;
        }
        if (esp_timer_get_time() >= deadline) {
            atomic_fetch_add(&s_stats.ack_timeouts, 1);
            invalidate_session();
            ESP_LOGW(TAG, "ACK queue stalled; host must repeat HELLO");
            return;
        }
    }
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    // These are esp_tinyusb task-context events, not custom DCD/ISR hooks.
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        atomic_store(&s_hello_seen, false);
        atomic_fetch_add(&s_generation, 1);
        atomic_store(&s_link_up, true);
        break;
    case TINYUSB_EVENT_DETACHED:
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
#endif
        atomic_store(&s_link_up, false);
        atomic_store(&s_hello_seen, false);
        atomic_fetch_add(&s_generation, 1);
        break;
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        atomic_store(&s_hello_seen, false);
        atomic_fetch_add(&s_generation, 1);
        atomic_store(&s_link_up, tud_mounted());
        break;
#endif
    default:
        break;
    }
    notify_receiver();
}

#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint16_t bytes)
#else
void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint32_t bytes)
#endif
{
    (void)interface;
    (void)buffer;
    (void)bytes;
    // Buffered TinyUSB owns the DMA buffer/FIFO; a separate task drains it.
    notify_receiver();
}

void tud_vendor_tx_cb(uint8_t interface, uint32_t bytes)
{
    (void)interface;
    (void)bytes;
    notify_receiver();
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request)
{
    if (request->bmRequestType == 0xC0 && request->bRequest == MS_OS_REQUEST &&
        request->wIndex == 7 && request->wValue == 0) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, request, (void *)&s_winusb_set, sizeof(s_winusb_set));
        }
        return true;
    }
    if (request->bmRequestType != 0xC1 || request->bRequest != USB_SCREEN_REQ_HELLO ||
        request->wValue != 0 || request->wIndex != 0 || request->wLength != sizeof(s_hello)) {
        return false;
    }
    if (stage == CONTROL_STAGE_SETUP) {
        // HELLO is informational during a running stream; after attach/stall it
        // enables the new generation. FIFO bytes are never cleared after this
        // response, which could otherwise discard an immediately arriving frame.
        atomic_store(&s_hello_seen, true);
        notify_receiver();
        return tud_control_xfer(rhport, request, (void *)&s_hello, sizeof(s_hello));
    }
    return true;
}

static bool valid_header(const usb_screen_frame_header_t *header)
{
    return header->magic == USB_SCREEN_FRAME_MAGIC && header->version == USB_SCREEN_VERSION &&
           header->header_bytes == sizeof(*header) && header->width == USB_SCREEN_WIDTH &&
           header->height == USB_SCREEN_HEIGHT && header->format == USB_SCREEN_FORMAT_JPEG &&
           header->flags == 0 && header->reserved == 0 && header->payload_bytes >= 4 &&
           header->payload_bytes <= USB_SCREEN_MAX_JPEG;
}

static void reset_parser(parser_t *parser, uint32_t generation)
{
    if (parser->buffer_index >= 0) {
        atomic_fetch_add(&s_stats.dropped, 1);
        return_jpeg(parser->buffer_index);
    }
    memset(parser, 0, sizeof(*parser));
    parser->buffer_index = -1;
    parser->generation = generation;
}

static void shift_header(parser_t *parser)
{
    --parser->header_used;
    memmove(parser->header_bytes, parser->header_bytes + 1, parser->header_used);
    atomic_fetch_add(&s_stats.resync_bytes, 1);
}

static size_t consume_bytes(parser_t *parser, const uint8_t *bytes, size_t length)
{
    static const uint8_t magic[] = {'W', 'P', '4', 'D'};
    size_t consumed = 0;
    while ((parser->job_pending || consumed < length) && session_live(parser->generation)) {
        if (parser->job_pending) {
            if (xQueueSend(s_frame_queue, &parser->pending_job, 0) != pdTRUE) break;
            queue_high_water(s_frame_queue, &s_stats.frame_queue_high);
            parser->job_pending = false;
            parser->buffer_index = -1; // ownership transferred to frame queue
            continue;
        }
        if (!parser->header_valid) {
            parser->header_bytes[parser->header_used++] = bytes[consumed++];
            parser->last_data_us = esp_timer_get_time();
            while (parser->header_used >= sizeof(magic) &&
                   memcmp(parser->header_bytes, magic, sizeof(magic)) != 0) {
                shift_header(parser);
            }
            if (parser->header_used != sizeof(parser->header_bytes)) continue;
            memcpy(&parser->header, parser->header_bytes, sizeof(parser->header));
            if (!valid_header(&parser->header)) {
                atomic_fetch_add(&s_stats.header_errors, 1);
                parser->pending_job = (frame_job_t){
                    .header = parser->header, .generation = parser->generation,
                    .status = USB_SCREEN_BAD_HEADER, .buffer_index = -1,
                };
                parser->job_pending = true;
                // Preserve the suffix so a valid header nested in a corrupt
                // candidate is found rather than skipping an arbitrary 32 bytes.
                shift_header(parser);
                continue;
            }
            parser->header_valid = true;
            parser->payload_used = 0;
            parser->crc = 0; // esp_rom_crc32_le uses zlib's complemented convention.
            continue;
        }
        if (parser->buffer_index < 0 &&
            xQueueReceive(s_free_queue, &parser->buffer_index, 0) != pdTRUE) break;
        size_t count = parser->header.payload_bytes - parser->payload_used;
        if (count > length - consumed) count = length - consumed;
        memcpy(s_jpeg[parser->buffer_index] + parser->payload_used, bytes + consumed, count);
        parser->crc = esp_rom_crc32_le(parser->crc, bytes + consumed, count);
        parser->payload_used += count;
        consumed += count;
        parser->last_data_us = esp_timer_get_time();
        if (parser->payload_used == parser->header.payload_bytes) {
            atomic_fetch_add(&s_stats.received, 1);
            uint32_t status = USB_SCREEN_OK;
            if (parser->crc != parser->header.payload_crc32) {
                status = USB_SCREEN_BAD_CRC;
                atomic_fetch_add(&s_stats.crc_errors, 1);
            }
            parser->pending_job = (frame_job_t){
                .header = parser->header, .generation = parser->generation,
                .status = status, .buffer_index = parser->buffer_index,
            };
            parser->job_pending = true;
            parser->header_valid = false;
            parser->header_used = 0;
        }
    }
    return consumed;
}

static void wait_until_started(void)
{
    while (!atomic_load(&s_started)) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void receive_task(void *arg)
{
    (void)arg;
    wait_until_started();
    parser_t parser = {.buffer_index = -1, .generation = UINT32_MAX};
    size_t chunk_used = 0, chunk_bytes = 0;
    ack_job_t pending_ack;
    bool ack_pending = false;
    int64_t ack_started_us = 0;
    unsigned drain_rounds = 0;
    for (;;) {
        bool progress = false;
        uint32_t generation = atomic_load(&s_generation);
        if (parser.generation != generation) {
            reset_parser(&parser, generation);
            chunk_used = chunk_bytes = 0;
            ack_pending = false;
            // Queued jobs are invalidated by generation. Workers return their
            // own buffers; do not reset a free queue while a worker owns one.
        }
        bool mounted = tud_mounted() && !tud_suspended() && tud_vendor_mounted();
        if (!mounted && atomic_exchange(&s_link_up, false)) invalidate_session();
        // Enumeration may finish while the wrapper is still installing its
        // callback. Polling can recover that mount without requiring a GPIO.
        if (mounted) atomic_store(&s_link_up, true);
        bool live = mounted && session_live(generation);

        if (!ack_pending) {
            while (xQueueReceive(s_ack_queue, &pending_ack, 0) == pdTRUE) {
                progress = true;
                if (live && pending_ack.generation == generation) {
                    ack_pending = true;
                    ack_started_us = esp_timer_get_time();
                    break;
                }
            }
        }
        // Only this task accesses the vendor read/write APIs. TinyUSB callbacks
        // and decoder/presenter tasks never compete for these application FIFOs.
        if (live && ack_pending) {
            if (tud_vendor_write_available() >= sizeof(pending_ack.wire)) {
                uint32_t sent = tud_vendor_write(&pending_ack.wire, sizeof(pending_ack.wire));
                if (sent == sizeof(pending_ack.wire)) {
                    tud_vendor_write_flush();
                    ack_pending = false;
                    progress = true;
                } else {
                    // With one application writer and enough space, partial
                    // enqueue violates this module's invariant: stop, don't
                    // append the whole ACK a second time and corrupt the stream.
                    atomic_fetch_add(&s_stats.ack_timeouts, 1);
                    invalidate_session();
                }
            } else if (esp_timer_get_time() - ack_started_us > PIPELINE_TIMEOUT_US) {
                atomic_fetch_add(&s_stats.ack_timeouts, 1);
                invalidate_session();
                ESP_LOGW(TAG, "Bulk IN stalled; host must read ACKs and repeat HELLO");
            }
        }

        if (live) {
            // Retry a complete frame blocked on the bounded worker queue even
            // when the last USB read ended exactly at the payload boundary.
            bool was_pending = parser.job_pending;
            consume_bytes(&parser, NULL, 0);
            if (was_pending && !parser.job_pending) progress = true;
            if (chunk_used < chunk_bytes) {
                size_t consumed = consume_bytes(&parser, s_rx_chunk + chunk_used, chunk_bytes - chunk_used);
                chunk_used += consumed;
                if (consumed != 0) progress = true;
            }
            if (chunk_used == chunk_bytes && !parser.job_pending && tud_vendor_available() != 0) {
                chunk_bytes = tud_vendor_read(s_rx_chunk, RX_CHUNK_BYTES);
                chunk_used = 0;
                atomic_fetch_add(&s_stats.bytes, chunk_bytes);
                chunk_used += consume_bytes(&parser, s_rx_chunk, chunk_bytes);
                if (chunk_bytes != 0) progress = true;
            }
            bool awaiting_wire = !parser.job_pending &&
                ((parser.header_valid && chunk_used == chunk_bytes) ||
                 (!parser.header_valid && parser.header_used != 0));
            if (awaiting_wire && esp_timer_get_time() - parser.last_data_us > FRAME_TIMEOUT_US) {
                atomic_fetch_add(&s_stats.timeouts, 1);
                reset_parser(&parser, generation);
                ESP_LOGW(TAG, "Partial frame timed out; searching for next WP4D header");
            }
        }
        // Notifications can coalesce. Continue draining available work after
        // real progress instead of adding 2 ms per 8 KiB. A bounded burst lets
        // the idle task run even during a continuous malformed input stream.
        bool more_work = live && session_live(generation) &&
            (chunk_used < chunk_bytes || parser.job_pending || tud_vendor_available() != 0 ||
             uxQueueMessagesWaiting(s_ack_queue) != 0);
        if (progress && more_work) {
            if (++drain_rounds >= 16) {
                drain_rounds = 0;
                vTaskDelay(1);
            }
            continue;
        }
        drain_rounds = 0;
        // Polling also notices a bus reset on a board without a VBUS sense GPIO.
        // Queue/FIFO backpressure is retained; no busy buffer is overwritten.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    }
}

static void decode_task(void *arg)
{
    (void)arg;
    wait_until_started();
    for (;;) {
        frame_job_t frame;
        if (xQueueReceive(s_frame_queue, &frame, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (!session_live(frame.generation)) {
            if (frame.buffer_index >= 0) return_jpeg(frame.buffer_index);
            atomic_fetch_add(&s_stats.dropped, 1);
            continue;
        }
        uint32_t status = frame.status, decode_us = 0;
        void *framebuffer = NULL;
        size_t framebuffer_bytes = 0;
        if (status == USB_SCREEN_OK) {
            esp_err_t err = wx101_panel_acquire_frame(&framebuffer, &framebuffer_bytes, pdMS_TO_TICKS(100));
            if (err == ESP_OK && session_live(frame.generation)) {
                err = usb_jpeg_decode(s_jpeg[frame.buffer_index], frame.header.payload_bytes,
                                      framebuffer, framebuffer_bytes, &decode_us);
                if (err == ESP_OK) {
                    atomic_fetch_add(&s_stats.decoded, 1);
                } else {
                    status = USB_SCREEN_DECODE_ERROR;
                    atomic_fetch_add(&s_stats.decode_errors, 1);
                }
            } else {
                status = USB_SCREEN_DROPPED;
                atomic_fetch_add(&s_stats.dropped, 1);
            }
        }
        if (frame.buffer_index >= 0) return_jpeg(frame.buffer_index);

        if (status == USB_SCREEN_OK && session_live(frame.generation)) {
            decoded_job_t decoded = {
                .framebuffer = framebuffer, .sequence = frame.header.sequence,
                .generation = frame.generation, .decode_us = decode_us,
            };
            int64_t deadline = esp_timer_get_time() + PIPELINE_TIMEOUT_US;
            bool queued = false;
            while (session_live(frame.generation) && esp_timer_get_time() < deadline) {
                if (xQueueSend(s_decoded_queue, &decoded, pdMS_TO_TICKS(20)) == pdTRUE) {
                    queue_high_water(s_decoded_queue, &s_stats.decoded_queue_high);
                    queued = true;
                    break;
                }
            }
            if (queued) continue; // framebuffer ownership passes to presenter.
            status = USB_SCREEN_DROPPED;
            atomic_fetch_add(&s_stats.dropped, 1);
        }
        if (framebuffer != NULL) wx101_panel_release_frame(framebuffer);
        if (session_live(frame.generation)) {
            ack_job_t ack = make_ack(frame.header.sequence, frame.generation, status, decode_us, 0);
            post_ack(&ack);
        }
    }
}

static void report_stats(void)
{
    static int64_t previous_us;
    static uint32_t previous_bytes, previous_rx, previous_decoded, previous_present, previous_refresh;
    int64_t now = esp_timer_get_time();
    if (previous_us == 0) {
        previous_us = now;
        previous_bytes = atomic_load(&s_stats.bytes);
        previous_rx = atomic_load(&s_stats.received);
        previous_decoded = atomic_load(&s_stats.decoded);
        previous_present = wx101_panel_present_count();
        previous_refresh = wx101_panel_refresh_count();
        return;
    }
    uint64_t elapsed = now - previous_us;
    if (elapsed < 1000000) return;
    uint32_t bytes = atomic_load(&s_stats.bytes), rx = atomic_load(&s_stats.received);
    uint32_t decoded = atomic_load(&s_stats.decoded), presented = wx101_panel_present_count();
    uint32_t refresh = wx101_panel_refresh_count();
    uint32_t rx_hz100 = (uint64_t)(rx - previous_rx) * 100000000 / elapsed;
    uint32_t dec_hz100 = (uint64_t)(decoded - previous_decoded) * 100000000 / elapsed;
    uint32_t present_hz100 = (uint64_t)(presented - previous_present) * 100000000 / elapsed;
    uint32_t refresh_hz100 = (uint64_t)(refresh - previous_refresh) * 100000000 / elapsed;
    ESP_LOGI(TAG, "USB_SCREEN rx_fps=%" PRIu32 ".%02" PRIu32 " decoded_fps=%" PRIu32 ".%02" PRIu32
             " present_fps=%" PRIu32 ".%02" PRIu32 " refresh_hz=%" PRIu32 ".%02" PRIu32
             " rx_Bps=%" PRIu32 " total_rx=%" PRIu32 " total_decoded=%" PRIu32 " total_present=%" PRIu32
             " crc_errors=%" PRIu32 " header_errors=%" PRIu32 " decode_errors=%" PRIu32
             " present_errors=%" PRIu32 " dropped=%" PRIu32 " timeouts=%" PRIu32 " ack_timeouts=%" PRIu32
             " q_high=%" PRIu32 " decoded_q_high=%" PRIu32 " resync_B=%" PRIu32,
             rx_hz100 / 100, rx_hz100 % 100, dec_hz100 / 100, dec_hz100 % 100,
             present_hz100 / 100, present_hz100 % 100, refresh_hz100 / 100, refresh_hz100 % 100,
             (uint32_t)((uint64_t)(bytes - previous_bytes) * 1000000 / elapsed), rx, decoded, presented,
             (uint32_t)atomic_load(&s_stats.crc_errors), (uint32_t)atomic_load(&s_stats.header_errors),
             (uint32_t)atomic_load(&s_stats.decode_errors), (uint32_t)atomic_load(&s_stats.present_errors),
             (uint32_t)atomic_load(&s_stats.dropped), (uint32_t)atomic_load(&s_stats.timeouts),
             (uint32_t)atomic_load(&s_stats.ack_timeouts), (uint32_t)atomic_load(&s_stats.frame_queue_high),
             (uint32_t)atomic_load(&s_stats.decoded_queue_high), (uint32_t)atomic_load(&s_stats.resync_bytes));
    previous_us = now;
    previous_bytes = bytes;
    previous_rx = rx;
    previous_decoded = decoded;
    previous_present = presented;
    previous_refresh = refresh;
}

static void present_task(void *arg)
{
    (void)arg;
    wait_until_started();
    for (;;) {
        report_stats();
        decoded_job_t decoded;
        if (xQueueReceive(s_decoded_queue, &decoded, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (!session_live(decoded.generation)) {
            wx101_panel_release_frame(decoded.framebuffer);
            atomic_fetch_add(&s_stats.dropped, 1);
            continue;
        }
        uint32_t present_us = 0;
        esp_err_t err = wx101_panel_present_frame(decoded.framebuffer, &present_us);
        uint32_t status = USB_SCREEN_OK;
        if (err != ESP_OK) {
            wx101_panel_release_frame(decoded.framebuffer);
            atomic_fetch_add(&s_stats.present_errors, 1);
            status = USB_SCREEN_DROPPED;
        }
        // Successful present has waited for the driver's vblank commit. This
        // counts actual buffer submissions taken over by scanout, not USB IRQs.
        if (session_live(decoded.generation)) {
            ack_job_t ack = make_ack(decoded.sequence, decoded.generation, status,
                                      decoded.decode_us, present_us);
            post_ack(&ack);
        }
    }
}

static void release_startup_resources(void)
{
    if (s_rx_task) vTaskDelete(s_rx_task);
    if (s_decode_task) vTaskDelete(s_decode_task);
    if (s_present_task) vTaskDelete(s_present_task);
    s_rx_task = s_decode_task = s_present_task = NULL;
    if (s_free_queue) vQueueDelete(s_free_queue);
    if (s_frame_queue) vQueueDelete(s_frame_queue);
    if (s_decoded_queue) vQueueDelete(s_decoded_queue);
    if (s_ack_queue) vQueueDelete(s_ack_queue);
    s_free_queue = s_frame_queue = s_decoded_queue = s_ack_queue = NULL;
    for (int i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        heap_caps_free(s_jpeg[i]);
        s_jpeg[i] = NULL;
    }
    heap_caps_free(s_rx_chunk);
    s_rx_chunk = NULL;
}

esp_err_t usb_screen_start(void)
{
    if (atomic_load(&s_started) || s_rx_task != NULL) return ESP_ERR_INVALID_STATE;
    // Lock down the IEEE/zlib wire convention before accepting host frames.
    static const uint8_t golden[] = "123456789";
    if (esp_rom_crc32_le(0, golden, sizeof(golden) - 1) != 0xCBF43926U) return ESP_ERR_INVALID_CRC;
    size_t alignment = 0;
    esp_err_t err = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment);
    if (err != ESP_OK) return err;
    if (alignment < 16) alignment = 16;
    for (int i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        s_jpeg[i] = heap_caps_aligned_alloc(alignment, USB_SCREEN_MAX_JPEG,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_jpeg[i] == NULL) goto no_memory;
    }
    s_rx_chunk = heap_caps_malloc(RX_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_free_queue = xQueueCreate(JPEG_BUFFER_COUNT, sizeof(int));
    s_frame_queue = xQueueCreate(FRAME_QUEUE_LENGTH, sizeof(frame_job_t));
    s_decoded_queue = xQueueCreate(DECODED_QUEUE_LENGTH, sizeof(decoded_job_t));
    s_ack_queue = xQueueCreate(ACK_QUEUE_LENGTH, sizeof(ack_job_t));
    if (!s_rx_chunk || !s_free_queue || !s_frame_queue || !s_decoded_queue || !s_ack_queue) goto no_memory;
    for (int i = 0; i < JPEG_BUFFER_COUNT; ++i) xQueueSend(s_free_queue, &i, 0);
    if (xTaskCreatePinnedToCore(receive_task, "usb_screen_rx", 6144, NULL, 9, &s_rx_task, 1) != pdPASS ||
        xTaskCreatePinnedToCore(decode_task, "usb_screen_jpeg", 8192, NULL, 6, &s_decode_task, 0) != pdPASS ||
        xTaskCreatePinnedToCore(present_task, "usb_screen_present", 6144, NULL, 7, &s_present_task, 0) != pdPASS) goto no_memory;

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event);
    config.port = TINYUSB_PORT_HIGH_SPEED_0;
    config.task.priority = 10;
    config.task.xCoreID = 1;
    config.descriptor.device = &s_device_descriptor;
    config.descriptor.qualifier = &s_qualifier;
    config.descriptor.string = s_strings;
    config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    config.descriptor.full_speed_config = s_fs_descriptor;
    config.descriptor.high_speed_config = s_hs_descriptor;
    err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        release_startup_resources();
        return err;
    }
    atomic_store(&s_started, true);
    xTaskNotifyGive(s_rx_task);
    xTaskNotifyGive(s_decode_task);
    xTaskNotifyGive(s_present_task);
    ESP_LOGI(TAG, "HUSB vendor %04X:%04X HS port=1, two %u-byte JPEG buffers, RX/JPEG/present pipeline",
             USB_SCREEN_VID, USB_SCREEN_PID, USB_SCREEN_MAX_JPEG);
    return ESP_OK;

no_memory:
    release_startup_resources();
    return ESP_ERR_NO_MEM;
}
