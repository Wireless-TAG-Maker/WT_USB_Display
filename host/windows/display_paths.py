"""Extend one newly plugged Amyuni target using Windows' Connecting and Configuring Displays API."""
import ctypes as C
from ctypes import wintypes as W
import sys

from topology import adapters, is_amyuni, user


class Luid(C.Structure):
    _fields_ = [("low", W.DWORD), ("high", W.LONG)]


class PathSource(C.Structure):
    _fields_ = [("adapter", Luid), ("id", W.UINT), ("mode_index", W.UINT), ("status", W.UINT)]


class PathTarget(C.Structure):
    _fields_ = [
        ("adapter", Luid), ("id", W.UINT), ("mode_index", W.UINT), ("technology", W.UINT),
        ("rotation", W.UINT), ("scaling", W.UINT), ("hz_num", W.UINT), ("hz_den", W.UINT),
        ("scan", W.UINT), ("available", W.BOOL), ("status", W.UINT),
    ]


class DisplayPath(C.Structure):
    _fields_ = [("source", PathSource), ("target", PathTarget), ("flags", W.UINT)]


class SourceMode(C.Structure):
    _fields_ = [("width", W.UINT), ("height", W.UINT), ("pixel_format", W.UINT), ("position", W.POINT)]


class ModeData(C.Union):
    # Preserve existing target timings and virtual desktop images verbatim.
    _fields_ = [("source", SourceMode), ("target_data", C.c_uint64 * 6)]


class DisplayMode(C.Structure):
    _fields_ = [("kind", W.UINT), ("id", W.UINT), ("adapter", Luid), ("data", ModeData)]


class DeviceHeader(C.Structure):
    _fields_ = [("kind", W.UINT), ("size", W.UINT), ("adapter", Luid), ("id", W.UINT)]


class SourceName(C.Structure):
    _fields_ = [("header", DeviceHeader), ("name", W.WCHAR * 32)]


class PreferredMode(C.Structure):
    _fields_ = [("header", DeviceHeader), ("width", W.UINT), ("height", W.UINT), ("target_data", C.c_uint64 * 6)]


assert (C.sizeof(DisplayPath), C.sizeof(DisplayMode), C.sizeof(PreferredMode)) == (72, 64, 80)
user.GetDisplayConfigBufferSizes.argtypes = [W.UINT, C.POINTER(W.UINT), C.POINTER(W.UINT)]
user.QueryDisplayConfig.argtypes = [
    W.UINT, C.POINTER(W.UINT), C.POINTER(DisplayPath), C.POINTER(W.UINT), C.POINTER(DisplayMode), C.c_void_p,
]
user.DisplayConfigGetDeviceInfo.argtypes = [C.c_void_p]
user.SetDisplayConfig.argtypes = [W.UINT, C.POINTER(DisplayPath), W.UINT, C.POINTER(DisplayMode), W.UINT]

PATH_ACTIVE = 1
PATH_SUPPORT_VIRTUAL_MODE = 8
INVALID_INDEX = 0xffffffff
# Windows 11 adds dynamic refresh rate to the modes that Query/SetDisplayConfig can preserve.
WINDOWS_11 = sys.getwindowsversion().platform_version[2] >= 22000
QUERY_FLAGS = 0x01 | 0x10 | (0x40 if WINDOWS_11 else 0)  # ALL_PATHS, VIRTUAL_MODE_AWARE, VIRTUAL_REFRESH_RATE_AWARE
SUPPLIED_FLAGS = 0x20 | 0x8000 | (0x20000 if WINDOWS_11 else 0)  # USE_SUPPLIED_DISPLAY_CONFIG plus the aware flags.
SDC_VALIDATE, SDC_APPLY = 0x40, 0x80


def key(endpoint):
    return (endpoint.adapter.high, endpoint.adapter.low, endpoint.id)


def device_info(record, kind, endpoint):
    record.header = DeviceHeader(kind, C.sizeof(record), endpoint.adapter, endpoint.id)
    result = user.DisplayConfigGetDeviceInfo(C.byref(record))
    if result:
        raise C.WinError(result)
    return record


class DisplayConfig:
    def __init__(self, paths, modes):
        self.paths = paths
        self.modes = modes
        self.active = [p for p in paths if p.flags & PATH_ACTIVE]
        self.available = {key(p.target) for p in paths if p.target.available}

    @classmethod
    def read(cls):
        n, m = W.UINT(), W.UINT()
        for _ in range(5):
            result = user.GetDisplayConfigBufferSizes(QUERY_FLAGS, C.byref(n), C.byref(m))
            if result:
                raise C.WinError(result)
            paths, modes = (DisplayPath * n.value)(), (DisplayMode * m.value)()
            result = user.QueryDisplayConfig(QUERY_FLAGS, C.byref(n), paths, C.byref(m), modes, None)
            if result != 122:  # ERROR_INSUFFICIENT_BUFFER if the monitor arrived between the two calls.
                break
        if result:
            raise C.WinError(result)
        return cls(paths[:n.value], modes[:m.value])

    def virtual_routes(self):
        # In a clone, the active source can be the Intel panel. ALL_PATHS also exposes the
        # virtual target's routes through its own adapter, including unused GDI sources.
        slots = {d.name for d in adapters() if is_amyuni(d)}
        names = {}
        routes = []
        for path in self.paths:
            if not path.target.available or bytes(path.source.adapter) != bytes(path.target.adapter):
                continue
            source = key(path.source)
            if source not in names:
                names[source] = device_info(SourceName(), 1, path.source).name
            if names[source] in slots:
                routes.append(path)
        return routes

    def source_layout(self, path):
        index = path.source.mode_index
        if path.flags & PATH_SUPPORT_VIRTUAL_MODE:
            index >>= 16
        if index < len(self.modes) and self.modes[index].kind == 1:
            mode = self.modes[index].data.source
            return mode.position.x, mode.position.y, mode.width, mode.height
        return None

    def share_desktop(self, first, second):
        if key(first.source) == key(second.source):
            return True
        a, b = self.source_layout(first), self.source_layout(second)
        return a is not None and b is not None and a[:2] == b[:2]

    def is_cloned(self, target):
        path = next((p for p in self.active if key(p.target) == target), None)
        if path is None:
            return False
        # Virtual-aware clones can have distinct source IDs but share the same desktop rectangle.
        return any(key(p.target) != target and self.share_desktop(path, p) for p in self.active)


def set_paths(paths, modes, operation):
    result = user.SetDisplayConfig(len(paths), (DisplayPath * len(paths))(*paths),
                                   len(modes), (DisplayMode * len(modes))(*modes), SUPPLIED_FLAGS | operation)
    if result:
        raise C.WinError(result)


class VirtualDisplayAddition:
    def __init__(self, desktop):
        self.before = DisplayConfig.read()
        self.already_present = bool(self.before.virtual_routes())
        right = max(desktop, key=lambda d: d["x"] + d["width"])
        self.position = W.POINT(right["x"] + right["width"], right["y"])
        self.target = None

    def try_extend(self):
        current = DisplayConfig.read()
        routes = [p for p in current.virtual_routes() if key(p.target) not in self.before.available]
        if not routes:
            new = current.available - self.before.available
            if len(new) == 1 and self.before.available < current.available:
                target, = new
                if current.is_cloned(target):
                    # Show the manual Extend action without applying a config to an unidentified adapter.
                    self.target = target
                    raise RuntimeError("Windows cloned the new target but did not expose its independent Amyuni route")
            return False
        added = {key(p.target) for p in routes}
        if len(added) != 1 or current.available - added != self.before.available:
            raise RuntimeError("Other connected targets changed while adding the virtual display")
        self.target, = added
        original = {key(p.target): p for p in self.before.active}
        if {key(p.target) for p in current.active} - added != original.keys():
            raise RuntimeError("Other active displays changed while adding the virtual display")
        new_path = next((p for p in current.active if key(p.target) == self.target), None)
        if new_path is not None and not current.is_cloned(self.target):
            return True  # Windows has already extended it. Wait for GDI to expose its desktop.
        for p in current.active:
            if new_path is not None and current.share_desktop(new_path, p):
                continue  # The clone itself can temporarily resize its original desktop.
            old = original[key(p.target)]
            if (current.source_layout(p) != self.before.source_layout(old) or
                    (p.target.rotation, p.target.hz_num, p.target.hz_den) !=
                    (old.target.rotation, old.target.hz_num, old.target.hz_den)):
                raise RuntimeError("Another display was reconfigured while adding the virtual display")
        in_use = {key(p.source) for p in self.before.active}
        route = next((p for p in routes if key(p.source) not in in_use), None)
        if route is None:
            raise RuntimeError("No independent source on the virtual display adapter")

        preferred = device_info(PreferredMode(), 3, route.target)
        mode = DisplayMode(kind=1, id=route.source.id, adapter=route.source.adapter)
        mode.data.source = SourceMode(preferred.width, preferred.height, 4, self.position)  # 32 bpp
        modes = [*self.before.modes, mode]
        path = DisplayPath.from_buffer_copy(route)
        index = len(modes) - 1
        path.source.mode_index = (index << 16 | 0xffff) if path.flags & PATH_SUPPORT_VIRTUAL_MODE else index
        path.target.mode_index = INVALID_INDEX  # Let Windows choose timings and desktop image for this target.
        path.target.rotation, path.target.scaling = 1, 128  # Landscape, preferred scaling.
        path.target.hz_num = path.target.hz_den = path.target.scan = 0
        path.flags = (path.flags & PATH_SUPPORT_VIRTUAL_MODE) | PATH_ACTIVE
        paths = [*self.before.active, path]
        # A supplied config keeps other displays' exact modes. Neither call saves a global display preset.
        set_paths(paths, modes, SDC_VALIDATE)
        set_paths(paths, modes, SDC_APPLY)
        return True

    def can_remove(self):
        # The driver only offers "remove last". Do not pop another application's monitor.
        targets = {key(p.target) for p in DisplayConfig.read().virtual_routes()}
        return not targets or (len(targets) == 1 and (self.target is None or self.target in targets))

    def target_available(self):
        return self.target in DisplayConfig.read().available
