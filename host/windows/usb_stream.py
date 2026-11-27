"""Enumerate the P4 through native Windows libusb and stream the selected desktop."""
import argparse
import ctypes as C
import ctypes.util
import json
import os
from contextlib import ExitStack, contextmanager
from pathlib import Path
import struct

from live_sender import run

VID_PID = (0xCAFE, 0x4011)
HELLO = struct.Struct("<4sHHHH5I")


def library_path(value=None):
    path = value or os.environ.get("WT_LIBUSB_DLL") or ctypes.util.find_library("libusb-1.0")
    if not path:
        raise RuntimeError("Set WT_LIBUSB_DLL to the 64-bit libusb-1.0.dll path")
    return str(Path(path).resolve(strict=True))


class USB:
    def __init__(self, library):
        self.dll = C.CDLL(library)
        pointer, integer, byte, word = C.c_void_p, C.c_int, C.c_ubyte, C.c_ushort
        pointer_list = C.POINTER(pointer)
        signatures = {
            "init": (integer, C.POINTER(pointer)),
            "exit": (None, pointer),
            "get_device_list": (C.c_ssize_t, pointer, C.POINTER(pointer_list)),
            "free_device_list": (None, pointer_list, integer),
            "get_device_descriptor": (integer, pointer, pointer),
            "get_device_speed": (integer, pointer),
            "open": (integer, pointer, C.POINTER(pointer)),
            "close": (None, pointer),
            "claim_interface": (integer, pointer, integer),
            "release_interface": (integer, pointer, integer),
            "control_transfer": (integer, pointer, byte, byte, word, word, pointer, word, C.c_uint),
            "bulk_transfer": (integer, pointer, byte, pointer, integer, C.POINTER(integer), C.c_uint),
            "error_name": (C.c_char_p, integer),
        }
        for name, (result_type, *argument_types) in signatures.items():
            function = getattr(self.dll, "libusb_" + name)
            function.restype = result_type
            function.argtypes = argument_types

    def check(self, name, code):
        if code < 0:
            raise OSError(f"{name}: {self.dll.libusb_error_name(code).decode()} ({code})")
        return code

    @contextmanager
    def devices(self):
        with ExitStack() as cleanup:
            context = C.c_void_p()
            self.check("libusb_init", self.dll.libusb_init(C.byref(context)))
            cleanup.callback(self.dll.libusb_exit, context)
            devices = C.POINTER(C.c_void_p)()
            count = self.check("get_device_list", self.dll.libusb_get_device_list(context, C.byref(devices)))
            cleanup.callback(self.dll.libusb_free_device_list, devices, 1)
            matches = []
            for index in range(count):
                device = devices[index]
                descriptor = C.create_string_buffer(18)
                if self.dll.libusb_get_device_descriptor(device, descriptor) == 0:
                    if struct.unpack_from("<HH", descriptor, 8) == VID_PID:
                        matches.append(device)
            yield matches

    @contextmanager
    def interface(self, device):
        if self.dll.libusb_get_device_speed(device) < 3:
            raise RuntimeError("HUSB requires a High-Speed USB connection")
        with ExitStack() as cleanup:
            handle = C.c_void_p()
            self.check("open", self.dll.libusb_open(device, C.byref(handle)))
            cleanup.callback(self.dll.libusb_close, handle)
            self.check("claim interface 0", self.dll.libusb_claim_interface(handle, 0))
            cleanup.callback(self.dll.libusb_release_interface, handle, 0)
            yield handle

    def hello(self, handle):
        data = C.create_string_buffer(HELLO.size)
        length = self.check("HELLO", self.dll.libusb_control_transfer(
            handle, 0xC1, 1, 0, 0, data, HELLO.size, 1500))
        if length != HELLO.size:
            raise OSError("Short P4 HELLO response")
        magic, version, size, width, height, max_jpeg, formats, *_ = HELLO.unpack(data)
        if (magic, version, size, width, height) != (b"WP4H", 1, 32, 800, 1280) or not formats & 2:
            raise OSError("Incompatible P4 capabilities")
        return min(max_jpeg, 768 * 1024)

    def transfer(self, handle, endpoint, data, length):
        actual = C.c_int()
        self.check(f"bulk {endpoint:#04x}", self.dll.libusb_bulk_transfer(
            handle, endpoint, data, length, C.byref(actual), 1500))
        return actual.value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="enumerate without claiming HUSB")
    parser.add_argument("--libusb", help="absolute path to 64-bit libusb-1.0.dll")
    parser.add_argument("--session", type=Path, default=Path(__file__).resolve().parent,
                        help="writable session directory")
    args = parser.parse_args()
    usb = USB(library_path(args.libusb))
    with usb.devices() as devices:
        if args.list:
            print("CAFE:4011 device count:", len(devices))
            for device in devices:
                print("USB speed (3 = High-Speed):", usb.dll.libusb_get_device_speed(device))
            return
        if len(devices) != 1:
            raise RuntimeError(f"Expected one CAFE:4011 at HUSB, found {len(devices)}")
        target = json.loads(os.environ["WT_P4_MONITOR"])
        with usb.interface(devices[0]) as handle:
            run(usb, handle, args.session, target, usb.hello(handle))


if __name__ == "__main__":
    main()
