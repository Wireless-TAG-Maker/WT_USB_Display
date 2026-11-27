#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstddef>

using Microsoft::WRL::ComPtr;

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) return hr; } while (false)

static HRESULT encodeFrame(const BYTE *pixels, INT stride, UINT width, UINT height, int quality,
                           BYTE *output, UINT capacity, UINT *written)
{
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!stream)
        return E_OUTOFMEMORY;
    ComPtr<IWICBitmapEncoder> encoder;
    CHECK(CoCreateInstance(CLSID_WICJpegEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)));
    CHECK(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    CHECK(encoder->CreateNewFrame(&frame, &options));
    PROPBAG2 properties[2]{};
    properties[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    properties[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    VARIANT values[2]{};
    values[0].vt = VT_R4;
    values[0].fltVal = quality / 100.0f;
    values[1].vt = VT_UI1;
    values[1].bVal = WICJpegYCrCbSubsampling444;
    CHECK(options->Write(2, properties, values));
    CHECK(frame->Initialize(options.Get()));
    CHECK(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    CHECK(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR)
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (stride > 0)
    {
        CHECK(frame->WritePixels(height, stride, stride * height, const_cast<BYTE *>(pixels)));
    }
    else
    {
        for (UINT y = 0; y < height; ++y)
        {
            const BYTE *row = pixels + static_cast<std::ptrdiff_t>(y) * stride;
            CHECK(frame->WritePixels(1, width * 3, width * 3, const_cast<BYTE *>(row)));
        }
    }
    CHECK(frame->Commit());
    CHECK(encoder->Commit());

    ULARGE_INTEGER length{};
    CHECK(stream->Seek({}, STREAM_SEEK_END, &length));
    if (length.QuadPart > capacity)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    CHECK(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    ULONG copied = 0;
    CHECK(stream->Read(output, length.LowPart, &copied));
    if (!copied || copied != length.LowPart)
        return STG_E_READFAULT;
    *written = copied;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT jpeg444_encode(const BYTE *pixels, INT stride, UINT width, UINT height,
                                                       int quality, BYTE *output, UINT capacity, UINT *written)
{
    if (!pixels || !output || !written || !capacity || width != 800 || height != 1280 ||
        quality < 1 || quality > 100 || (stride > -static_cast<INT>(width * 3) && stride < static_cast<INT>(width * 3)) ||
        stride > static_cast<INT>(UINT_MAX / height))
        return E_INVALIDARG;
    *written = 0;
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
        return apartment;
    HRESULT hr;
    for (;;)
    {
        hr = encodeFrame(pixels, stride, width, height, quality, output, capacity, written);
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || quality == 1)
            break;
        // Preserve 4:4:4 on unusually detailed frames that exceed the device's payload limit.
        quality = quality > 12 ? quality - 12 : 1;
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return hr;
}
