#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwctype>
#include <string>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void chk(HRESULT hr, const char *op)
{
    if (FAILED(hr))
    {
        char text[220];
        sprintf_s(text, "%s HRESULT 0x%08lx", op, (unsigned long)hr);
        throw std::runtime_error(text);
    }
}
#define OK(expr) chk((expr), #expr)
static std::string fourcc(GUID g)
{
    std::string s;
    for (int i = 0; i < 4; i++)
    {
        char ch = (char)((g.Data1 >> (8 * i)) & 255);
        s += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return s;
}
static void print_type(DWORD index, IMFMediaType *t)
{
    UINT32 w = 0, h = 0, a = 0, b = 0;
    GUID g{};
    OK(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h));
    OK(t->GetGUID(MF_MT_SUBTYPE, &g));
    OK(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &a, &b));
    printf("mode=%lu format=%s width=%u height=%u fps=%u/%u\n", (unsigned long)index, fourcc(g).c_str(), w,
           h, a, b);
}
static int run(int argc, wchar_t **argv)
{
    const wchar_t *requested = _wgetenv(L"WT_UVC_ID");
    std::wstring id = requested && *requested ? requested : L"1bcf:2d4f";
    if (id.size() != 9 || id[4] != L':' ||
        id.find_first_not_of(L"0123456789abcdefABCDEF:") != std::wstring::npos || id.find(L':') != 4 ||
        id.rfind(L':') != 4)
        throw std::runtime_error("WT_UVC_ID must be VID:PID in hex");
    std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    const std::wstring selector = L"vid_" + id.substr(0, 4) + L"&pid_" + id.substr(5);
    ComPtr<IMFAttributes> attrs;
    OK(MFCreateAttributes(&attrs, 1));
    OK(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    IMFActivate **devs = nullptr;
    UINT32 count = 0;
    OK(MFEnumDeviceSources(attrs.Get(), &devs, &count));
    ComPtr<IMFActivate> found;
    int hits = 0;
    for (UINT32 i = 0; i < count; i++)
    {
        WCHAR *link = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                  &link, &len)))
        {
            std::wstring path(link, len);
            CoTaskMemFree(link);
            std::transform(path.begin(), path.end(), path.begin(),
                           [](wchar_t c) { return (wchar_t)towlower(c); });
            if (path.find(selector) != std::wstring::npos)
            {
                found = devs[i];
                hits++;
            }
        }
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    if (hits != 1)
        throw std::runtime_error("Expected one video camera matching WT_UVC_ID");
    ComPtr<IMFMediaSource> source;
    OK(found->ActivateObject(IID_PPV_ARGS(&source)));
    ComPtr<IMFAttributes> ra;
    OK(MFCreateAttributes(&ra, 2));
    OK(ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));
    OK(ra->SetUINT32(MF_LOW_LATENCY, TRUE));
    ComPtr<IMFSourceReader> sr;
    OK(MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &sr));
    const DWORD stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    if (argc == 2 && !wcscmp(argv[1], L"list"))
    {
        for (DWORD i = 0;; i++)
        {
            ComPtr<IMFMediaType> t;
            HRESULT hr = sr->GetNativeMediaType(stream, i, &t);
            if (hr == MF_E_NO_MORE_TYPES)
                break;
            OK(hr);
            print_type(i, t.Get());
        }
    }
    else if (argc == 5 && !wcscmp(argv[1], L"record"))
    {
        int i = _wtoi(argv[2]);
        double seconds = _wtof(argv[3]);
        if (i < 0 || seconds <= 0 || seconds > 200)
            throw std::runtime_error("Invalid capture settings");
        ComPtr<IMFMediaType> t;
        OK(sr->GetNativeMediaType(stream, i, &t));
        OK(sr->SetCurrentMediaType(stream, nullptr, t.Get()));
        OK(sr->SetStreamSelection(stream, TRUE));
        t.Reset();
        OK(sr->GetCurrentMediaType(stream, &t));
        print_type(i, t.Get());
        GUID subtype{};
        OK(t->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (subtype != MFVideoFormat_MJPG)
            throw std::runtime_error("Select a native MJPG camera mode");
        std::wstring base = argv[4];
        FILE *video = nullptr, *csv = nullptr;
        const auto video_path = base + L".bin";
        if (_wfopen_s(&video, video_path.c_str(), L"wbx"))
            throw std::runtime_error("Cannot create MJPEG recording");
        std::unique_ptr<FILE, decltype(&fclose)> video_file(video, fclose);
        if (_wfopen_s(&csv, (base + L".csv").c_str(), L"wbx"))
        {
            video_file.reset();
            _wremove(video_path.c_str());
            throw std::runtime_error("Cannot create camera timestamp index");
        }
        std::unique_ptr<FILE, decltype(&fclose)> csv_file(csv, fclose);
        setvbuf(video, nullptr, _IOFBF, 1 << 20);
        LARGE_INTEGER freq, now, begin;
        QueryPerformanceFrequency(&freq);
        fprintf(csv, "frame,sample_100ns,arrival_qpc,qpc_hz,offset,length,flags\n");
        long long offset = 0;
        int frames = 0, streamTicks = 0;
        for (;;)
        {
            DWORD flags = 0, actual = 0;
            LONGLONG ts;
            ComPtr<IMFSample> sample;
            OK(sr->ReadSample(stream, 0, &actual, &flags, &ts, &sample));
            QueryPerformanceCounter(&now);
            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM |
                         MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
                throw std::runtime_error("Camera stream stopped or changed mode");
            if (!sample)
            {
                streamTicks++;
                continue;
            }
            if (!frames)
                begin = now;
            ComPtr<IMFMediaBuffer> mb;
            OK(sample->ConvertToContiguousBuffer(&mb));
            BYTE *buf;
            DWORD n = 0;
            OK(mb->Lock(&buf, nullptr, &n));
            bool written = fwrite(buf, 1, n, video) == n;
            mb->Unlock();
            if (!written)
                throw std::runtime_error("Recording write failed");
            fprintf(csv, "%d,%lld,%lld,%lld,%lld,%lu,%lu\n", frames, ts, now.QuadPart, freq.QuadPart,
                    offset, (unsigned long)n, (unsigned long)flags);
            if (frames++ % 24 == 0)
                fflush(csv);
            offset += n;
            if (double(now.QuadPart - begin.QuadPart) / freq.QuadPart >= seconds)
                break;
        }
        if (fflush(video) != 0 || fflush(csv) != 0)
            throw std::runtime_error("Unable to flush camera capture");
        printf("frames=%d bytes=%lld elapsed_s=%.4f stream_ticks=%d\n", frames, offset,
               double(now.QuadPart - begin.QuadPart) / freq.QuadPart, streamTicks);
    }
    else
        throw std::runtime_error("Camera list | record MODE SECONDS PREFIX");
    sr.Reset();
    source->Shutdown();
    found->ShutdownObject();
    return 0;
}
int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION)))
        return 2;
    int rc = 1;
    try
    {
        rc = run(argc, argv);
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "%s\n", e.what());
    }
    MFShutdown();
    CoUninitialize();
    return rc;
}
