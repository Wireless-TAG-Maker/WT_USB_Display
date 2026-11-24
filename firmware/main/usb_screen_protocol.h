#pragma once

#include <stdint.h>

/* Development-only TinyUSB test VID/PID. All fields are little-endian. */
#define USB_SCREEN_VID 0xCAFE
#define USB_SCREEN_PID 0x4011
#define USB_SCREEN_VERSION 1
#define USB_SCREEN_WIDTH 800
#define USB_SCREEN_HEIGHT 1280
#define USB_SCREEN_MAX_JPEG (768U * 1024U)
#define USB_SCREEN_EP_OUT 0x01
#define USB_SCREEN_EP_IN 0x81
#define USB_SCREEN_REQ_HELLO 0x01
#define USB_SCREEN_FRAME_MAGIC 0x44345057U /* WP4D */
#define USB_SCREEN_ACK_MAGIC 0x41345057U   /* WP4A */
#define USB_SCREEN_HELLO_MAGIC 0x48345057U /* WP4H */
#define USB_SCREEN_FORMAT_JPEG 1

enum {
    USB_SCREEN_OK = 0,
    USB_SCREEN_BAD_HEADER = 1,
    USB_SCREEN_BAD_CRC = 2,
    USB_SCREEN_DECODE_ERROR = 3,
    USB_SCREEN_DROPPED = 4,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;
    uint32_t payload_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t flags;
    uint32_t payload_crc32; /* IEEE CRC32, same as zlib crc32(0,...). */
    uint32_t reserved;
} usb_screen_frame_header_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;
    uint32_t status;
    uint32_t received_frames;
    uint32_t decoded_frames;
    uint32_t presented_frames;
    uint32_t decode_us;
    uint32_t present_us;
    uint32_t host_refresh_events;
} usb_screen_ack_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint16_t width;
    uint16_t height;
    uint32_t max_jpeg_bytes;
    uint32_t formats;
    uint32_t preferred_refresh_millihz;
    uint32_t capabilities;
    uint32_t reserved;
} usb_screen_hello_t;
#pragma pack(pop)

_Static_assert(sizeof(usb_screen_frame_header_t) == 32, "Frame header is 32 bytes");
_Static_assert(sizeof(usb_screen_ack_t) == 40, "ACK is 40 bytes");
_Static_assert(sizeof(usb_screen_hello_t) == 32, "HELLO is 32 bytes");
