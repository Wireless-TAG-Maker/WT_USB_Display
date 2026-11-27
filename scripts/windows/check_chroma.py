"""Exercise the production Windows 4:4:4 encoder offline. Pass the path to GpuCapture.dll."""
import ctypes as C
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host/windows"))
from live_sender import jpeg_chroma


def check(dll_path):
    jpeg = C.CDLL(str(dll_path.resolve())).jpeg444_encode
    jpeg.argtypes = (C.c_void_p, C.c_int, C.c_uint, C.c_uint, C.c_int,
                     C.c_void_p, C.c_uint, C.POINTER(C.c_uint))
    jpeg.restype = C.c_int32
    width, height = 800, 1280
    stride = width * 3
    cap = 768 * 1024
    pixels = random.Random(42).randbytes(stride * height)

    def encode(source, row_stride, capacity):
        raw = C.create_string_buffer(source)
        first = 0 if row_stride > 0 else (height - 1) * stride
        output = C.create_string_buffer(capacity + 16)
        output[capacity:] = b"GUARD" * 3 + b"!"
        written = C.c_uint()
        hr = jpeg(C.byref(raw, first), row_stride, width, height, 72, output, capacity, C.byref(written))
        assert output[capacity:] == b"GUARD" * 3 + b"!"
        assert written.value <= capacity
        return hr, output[:written.value]

    hr, normal = encode(pixels, stride, len(pixels))
    assert hr == 0 and len(normal) > cap and jpeg_chroma(normal) == "4:4:4"
    hr, limited = encode(pixels, stride, cap)
    assert hr == 0 and 0 < len(limited) <= cap and jpeg_chroma(limited) == "4:4:4"
    bottom_up = b"".join(pixels[y * stride:(y + 1) * stride] for y in reversed(range(height)))
    assert encode(bottom_up, -stride, cap) == (0, limited)
    hr, empty = encode(pixels, stride, 64)
    assert hr < 0 and not empty
    assert encode(pixels, stride, len(pixels)) == (0, normal)
    print(f"4:4:4 JPEG: {len(normal)} bytes at quality 72, {len(limited)} within the P4 limit")


if __name__ == "__main__":
    check(Path(sys.argv[1]))
