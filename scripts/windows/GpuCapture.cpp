#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <cwchar>
#include <memory>
using Microsoft::WRL::ComPtr;
static constexpr UINT width = 800, height = 1280;
#define CHECK(call)                                                                                        \
    do                                                                                                     \
    {                                                                                                      \
        HRESULT result = (call);                                                                           \
        if (FAILED(result))                                                                                \
            return result;                                                                                 \
    } while (false)

struct Capture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> input, target, staging;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    ComPtr<ID3D11RenderTargetView> targetView;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;

    bool cursorSeparate = false;

    HRESULT init(const wchar_t *name, const RECT &bounds)
    {
        ComPtr<IDXGIFactory1> factory;
        CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> owner;
        ComPtr<IDXGIOutput1> monitor;
        for (UINT ai = 0; !monitor; ai++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(ai, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            CHECK(hr);
            for (UINT oi = 0;; oi++)
            {
                ComPtr<IDXGIOutput> output;
                hr = adapter->EnumOutputs(oi, &output);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                CHECK(hr);
                DXGI_OUTPUT_DESC d{};
                CHECK(output->GetDesc(&d));
                if (d.AttachedToDesktop && wcscmp(d.DeviceName, name) == 0 &&
                    EqualRect(&d.DesktopCoordinates, &bounds))
                {
                    CHECK(output.As(&monitor));
                    owner = adapter;
                    break;
                }
            }
        }
        if (!monitor)
            return DXGI_ERROR_NOT_FOUND;
        CHECK(D3D11CreateDevice(owner.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        CHECK(monitor->DuplicateOutput(device.Get(), &duplication));
        DXGI_OUTDUPL_DESC dd{};
        duplication->GetDesc(&dd);
        if (dd.ModeDesc.Width != static_cast<UINT>(bounds.right - bounds.left) ||
            dd.ModeDesc.Height != static_cast<UINT>(bounds.bottom - bounds.top) ||
            dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || dd.Rotation != DXGI_MODE_ROTATION_IDENTITY)
            return E_UNEXPECTED;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dd.ModeDesc.Format;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK(device->CreateTexture2D(&td, nullptr, &input));
        CHECK(device->CreateShaderResourceView(input.Get(), nullptr, &sourceView));
        td.Width = width;
        td.Height = height;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK(device->CreateTexture2D(&td, nullptr, &target));
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &targetView));
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&td, nullptr, &staging));
        const char shader[] = R"(
            Texture2D src:register(t0); SamplerState linearClamp:register(s0);
            float4 vs(uint i:SV_VertexID):SV_POSITION {
                return float4(i==1 ? 3.0 : -1.0,i==2 ? -3.0 : 1.0,0,1);
            }
            float4 ps(float4 p:SV_POSITION):SV_Target {
                return src.Sample(linearClamp,float2(1-p.y/1280.0,p.x/800.0));
            })";
        ComPtr<ID3DBlob> vs, ps;
        CHECK(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs,
                         nullptr));
        CHECK(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps,
                         nullptr));
        CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
        CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        CHECK(device->CreateSamplerState(&sd, &sampler));
        D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, sourceView.GetAddressOf());
        context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
        return S_OK;
    }
    HRESULT next(BYTE *bytes, int length)
    {
        if (!bytes || length != width * height * 4)
            return E_INVALIDARG;
        ComPtr<IDXGIResource> data;
        DXGI_OUTDUPL_FRAME_INFO info{};
        HRESULT hr = duplication->AcquireNextFrame(45, &info, &data);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return S_FALSE;
        CHECK(hr);
        struct Frame
        {
            IDXGIOutputDuplication *d;
            ~Frame()
            {
                d->ReleaseFrame();
            }
        } frame{duplication.Get()};
        if (info.LastMouseUpdateTime.QuadPart)
            cursorSeparate = (info.PointerPosition.Visible != FALSE);
        if (!info.LastPresentTime.QuadPart)
            return S_FALSE;
        ComPtr<ID3D11Texture2D> source;
        CHECK(data.As(&source));
        context->CopyResource(input.Get(), source.Get());
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
        for (UINT y = 0; y < height; y++)
            memcpy(bytes + y * width * 4, static_cast<const BYTE *>(m.pData) + y * m.RowPitch, width * 4);
        context->Unmap(staging.Get(), 0);
        return S_OK;
    }
};
extern "C" __declspec(dllexport) HRESULT capture_open(const wchar_t *name, int x, int y, int w, int h,
                                                      Capture **out)
{
    if (!out || !name || w <= 0 || h <= 0)
        return E_INVALIDARG;
    *out = nullptr;
    auto cap = std::make_unique<Capture>();
    RECT bounds{x, y, x + w, y + h};
    CHECK(cap->init(name, bounds));
    *out = cap.release();
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT capture_next(Capture *cap, BYTE *bytes, int n)
{
    return cap ? cap->next(bytes, n) : E_INVALIDARG;
}
extern "C" __declspec(dllexport) void capture_close(Capture *cap)
{
    delete cap;
}

extern "C" __declspec(dllexport) int capture_cursor_separate(Capture *cap)
{
    return cap && cap->cursorSeparate;
}
