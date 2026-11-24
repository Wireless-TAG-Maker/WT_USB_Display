"""Inspect active Windows displays and select a landscape mode on the Amyuni adapter."""
import ctypes as C
from ctypes import wintypes as W
import json


class DisplayDevice(C.Structure):
    _fields_ = [
        ("size", W.DWORD), ("name", W.WCHAR * 32), ("label", W.WCHAR * 128),
        ("flags", W.DWORD), ("id", W.WCHAR * 128), ("key", W.WCHAR * 128),
    ]


class DevMode(C.Structure):
    _fields_ = [
        ("name", W.WCHAR * 32), ("spec", W.WORD), ("driver", W.WORD), ("size", W.WORD),
        ("extra", W.WORD), ("fields", W.DWORD), ("x", W.LONG), ("y", W.LONG),
        ("orientation", W.DWORD), ("fixed", W.DWORD), ("color", W.SHORT), ("duplex", W.SHORT),
        ("yres", W.SHORT), ("tt", W.SHORT), ("collate", W.SHORT), ("form", W.WCHAR * 32),
        ("logpixels", W.WORD),
    ] + [(key, W.DWORD) for key in (
        "bpp", "width", "height", "flags", "freq", "icm", "intent", "media", "dither",
        "res1", "res2", "panw", "panh",
    )]


assert C.sizeof(DevMode) == 220
user = C.WinDLL("user32", use_last_error=True)
user.SetProcessDpiAwarenessContext(C.c_void_p(-4))
user.EnumDisplayDevicesW.argtypes = [W.LPCWSTR, W.DWORD, C.POINTER(DisplayDevice), W.DWORD]
user.EnumDisplaySettingsExW.argtypes = [W.LPCWSTR, W.DWORD, C.POINTER(DevMode), W.DWORD]
user.ChangeDisplaySettingsExW.argtypes = [W.LPCWSTR, C.POINTER(DevMode), W.HWND, W.DWORD, C.c_void_p]


def devs():
    devices = []
    index = 0
    while True:
        device = DisplayDevice(size=C.sizeof(DisplayDevice))
        if not user.EnumDisplayDevicesW(None, index, C.byref(device), 0):
            break
        index += 1
        if not device.flags & 1:  # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
            continue
        mode = DevMode(size=C.sizeof(DevMode))
        if not user.EnumDisplaySettingsExW(device.name, 0xFFFFFFFF, C.byref(mode), 0):
            raise RuntimeError("Cannot read current mode for " + device.name)
        devices.append(dict(name=device.name, adapter=device.label, primary=bool(device.flags & 4),
                            x=mode.x, y=mode.y, width=mode.width, height=mode.height,
                            hz=mode.freq, orientation=mode.orientation))
    return sorted(devices, key=lambda d: d["name"])


def modes(name):
    index = 0
    while True:
        mode = DevMode(size=C.sizeof(DevMode))
        if not user.EnumDisplaySettingsExW(name, index, C.byref(mode), 0):
            return
        yield mode
        index += 1


def set_test_mode():
    names = [d["name"] for d in devs() if d["adapter"] == "USB Mobile Monitor Virtual Display" and not d["primary"]]
    if len(names) != 1:
        raise RuntimeError("Expected one secondary Amyuni monitor")
    name, = names
    available = list(modes(name))
    for width, height in ((1280, 800), (1440, 900), (1920, 1200)):
        matches = [m for m in available if (m.width, m.height, m.bpp) == (width, height, 32)]
        if matches:
            mode = min(matches, key=lambda m: abs(m.freq - 60))
            mode.fields = 0x80000 | 0x100000 | 0x400000  # width, height, frequency
            for flags in (2, 0):  # CDS_TEST, then apply only to this output
                result = user.ChangeDisplaySettingsExW(name, C.byref(mode), None, flags, None)
                if result:
                    raise RuntimeError(f"ChangeDisplaySettingsExW returned {result}")
            return
    raise RuntimeError("No supported 16:10 mode on the virtual monitor")


if __name__ == "__main__":
    print(json.dumps(devs(), indent=2))
