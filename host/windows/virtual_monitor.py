"""Use the installed Amyuni monitor control interface, as used by RustDesk 1.5.3."""
import ctypes as C
from ctypes import wintypes as W
from uuid import UUID


def command(action):
    if action not in ("probe", "add", "remove"):
        raise ValueError(action)
    guid = (C.c_ubyte * 16).from_buffer_copy(UUID("b5ffd75f-da40-4353-8ff8-b6daf6f1d8ca").bytes_le)
    manager = C.WinDLL("cfgmgr32")
    size = W.ULONG()
    result = manager.CM_Get_Device_Interface_List_SizeW(C.byref(size), guid, None, 0)
    if result:
        raise OSError(f"Cannot size the Amyuni device interface list ({result})")
    buffer = C.create_unicode_buffer(size.value)
    result = manager.CM_Get_Device_Interface_ListW(guid, None, buffer, size.value, 0)
    if result:
        raise OSError(f"Cannot enumerate Amyuni device interfaces ({result})")
    paths = [path for path in buffer[:].split("\0") if path]
    if len(paths) != 1:
        raise RuntimeError(f"Expected one Amyuni device interface, found {len(paths)}")

    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.LPVOID, W.DWORD, W.DWORD, W.HANDLE]
    kernel.CreateFileW.restype = W.HANDLE
    kernel.DeviceIoControl.argtypes = [
        W.HANDLE, W.DWORD, W.LPVOID, W.DWORD, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), W.LPVOID,
    ]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.CreateFileW(paths[0], 0xC0000000, 3, None, 3, 0, None)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        if action != "probe":
            request = W.DWORD(0x10 if action == "add" else 0)
            written = W.DWORD()
            if not kernel.DeviceIoControl(handle, 2307084, C.byref(request), C.sizeof(request),
                                          None, 0, C.byref(written), None):
                raise C.WinError(C.get_last_error())
    finally:
        kernel.CloseHandle(handle)
