#include "hooks.h"
#include "render.h"
#include <MinHook.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace hooks {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn oPresent = nullptr;
static ResizeFn oResize = nullptr;
static bool g_init = false;
static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static void create_rtv(IDXGISwapChain* sc) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* backbuf = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&backbuf));
    if (backbuf) {
        g_device->CreateRenderTargetView(backbuf, nullptr, &g_rtv);
        backbuf->Release();
    }
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_init) {
        sc->GetDevice(IID_PPV_ARGS(&g_device));
        if (g_device) {
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            g_hwnd = desc.OutputWindow;
            create_rtv(sc);
            render::init(g_hwnd, g_device, g_context);
            g_init = true;
        }
    }
    if (g_init && g_rtv) {
        render::begin_frame(g_hwnd);
        render::draw(g_context, g_rtv);
        render::end_frame(g_context, g_rtv);
    }
    return oPresent(sc, sync, flags);
}

static HRESULT WINAPI hkResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    HRESULT hr = oResize(sc, count, w, h, fmt, flags);
    if (g_init && SUCCEEDED(hr)) create_rtv(sc);
    return hr;
}

static bool get_vtable(void** out) {
    // Create temporary D3D11 device + swapchain to grab vtable
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0,
                   GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"RRtmp", nullptr};
    RegisterClassExW(&wc);
    HWND tmp = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                               0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = tmp;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        DestroyWindow(tmp);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(sc);
    out[0] = vtable[8];  // Present
    out[1] = vtable[13]; // ResizeBuffers
    sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(tmp);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return true;
}

bool install() {
    void* targets[2]{};
    if (!get_vtable(targets)) return false;
    MH_Initialize();
    MH_CreateHook(targets[0], (void*)&hkPresent, (void**)&oPresent);
    MH_CreateHook(targets[1], (void*)&hkResize, (void**)&oResize);
    MH_EnableHook(targets[0]);
    MH_EnableHook(targets[1]);
    return true;
}

void uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    render::shutdown();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_init = false;
}

bool is_initialized() { return g_init; }
HWND get_hwnd() { return g_hwnd; }

} // namespace hooks