#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "console.h"
#include "game.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// We include ImGui sources directly in the DLL build
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {

// State
inline bool g_initialized = false;
inline bool g_show = true;
inline ImGuiContext* g_ctx = nullptr;
inline ID3D11Device* g_d3d11_device = nullptr;
inline ID3D11DeviceContext* g_d3d11_context = nullptr;
inline ID3D11RenderTargetView* g_rtv = nullptr;
inline IDXGISwapChain* g_swap_chain = nullptr;
inline HWND g_hwnd = nullptr;
inline WNDPROC g_orig_wndproc = nullptr;

// Original Flip function
using Flip_t = bool(__cdecl*)(const void*, const void*, HWND);
inline Flip_t g_orig_flip = nullptr;

// WndProc hook for input
inline LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_show = !g_show;
        return 0;
    }
    if (g_show && g_ctx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_ctx);
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            ImGui::SetCurrentContext(prev);
            return 0;
        }
        ImGui::SetCurrentContext(prev);
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wParam, lParam);
}

// Draw our overlay
inline void draw() {
    if (!g_show) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 350), ImGuiCond_FirstUseEver);
    ImGui::Begin("Bot [INS]", &g_show);

    auto name = game::get_name();
    int hp = game::get_hp(), max_hp = game::get_max_hp();
    int mp = game::get_mp(), sta = game::get_stamina();
    int x = 0, y = 0; game::get_pos(x, y);

    ImGui::Text("%s  ID:%u", name.c_str(), game::get_id());
    if (max_hp > 0) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.15f, 0.15f, 1));
        ImGui::ProgressBar((float)hp / (float)max_hp, ImVec2(-1, 14));
        ImGui::PopStyleColor();
    }
    ImGui::Text("HP:%d/%d  MP:%d  Sta:%d/%d", hp, max_hp, mp, sta, game::get_max_stamina());
    ImGui::Text("Pos:%d,%d  Silver:%lld", x, y, game::get_silver());

    ImGui::Separator();
    static int jx = 0, jy = 0;
    ImGui::SetNextItemWidth(60); ImGui::InputInt("##jx", &jx);
    ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt("##jy", &jy);
    ImGui::SameLine();
    if (ImGui::Button("Jump")) game::queue_jump(jx, jy);
    ImGui::SameLine();
    if (ImGui::Button("Here")) { jx = x; jy = y; }

    ImGui::End();
}

// Find the game window
inline HWND find_game_window() {
    struct FindData { DWORD pid; HWND hwnd; } fd{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<FindData*>(lp);
        DWORD wp = 0; GetWindowThreadProcessId(hwnd, &wp);
        if (wp == d->pid && IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
            d->hwnd = hwnd; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fd));
    return fd.hwnd;
}

inline bool try_init() {
    uintptr_t base = game::get_base();

    // Read g_pD3DDevice from graphic.dll import
    uintptr_t device_ptr_addr = 0;
    game::safe_read(base + 0x408E68, device_ptr_addr);
    if (!device_ptr_addr || !game::is_valid_ptr(device_ptr_addr)) {
        console::warn("Overlay: g_pD3DDevice import not found");
        return false;
    }

    uintptr_t raw_device = 0;
    game::safe_read(device_ptr_addr, raw_device);
    if (!raw_device) {
        console::warn("Overlay: D3D device is null");
        return false;
    }

    IUnknown* unk = reinterpret_cast<IUnknown*>(raw_device);

    // Get D3D11 device
    HRESULT hr = unk->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_d3d11_device));
    if (FAILED(hr) || !g_d3d11_device) {
        console::error("Overlay: device is not D3D11 (hr=0x%X)", hr);
        return false;
    }
    g_d3d11_device->GetImmediateContext(&g_d3d11_context);
    console::ok("Game device IS D3D11");

    // Get the DXGI swap chain through the device -> adapter -> factory chain
    IDXGIDevice* dxgi_dev = nullptr;
    g_d3d11_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
    if (!dxgi_dev) { console::error("No DXGI device"); return false; }

    IDXGIAdapter* adapter = nullptr;
    dxgi_dev->GetAdapter(&adapter);
    dxgi_dev->Release();
    if (!adapter) { console::error("No adapter"); return false; }

    // Try IDXGIFactory1 first, fallback to IDXGIFactory
    IDXGIFactory* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr))
        hr = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    adapter->Release();

    // If GetParent fails, create factory directly
    if (FAILED(hr) || !factory) {
        hr = CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            console::error("Cannot get/create DXGI factory (hr=0x%X)", hr);
            return false;
        }
    }

    // Find the game window
    g_hwnd = find_game_window();
    if (!g_hwnd) { console::error("No game window"); factory->Release(); return false; }
    console::info("Game window: 0x%llX", (uintptr_t)g_hwnd);

    // Create our own swap chain on the game window using the same factory+device
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 0;  // auto
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = factory->CreateSwapChain(g_d3d11_device, &sd, &g_swap_chain);
    factory->Release();

    if (FAILED(hr) || !g_swap_chain) {
        console::error("CreateSwapChain failed: 0x%X", hr);
        return false;
    }
    console::ok("Created overlay swap chain");

    // Create RTV from swap chain back buffer
    ID3D11Texture2D* bb = nullptr;
    g_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (bb) {
        g_d3d11_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }

    if (!g_rtv) { console::error("No RTV"); return false; }

    // Create ImGui context
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.92f;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d3d11_device, g_d3d11_context);

    // Hook WndProc
    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked_wndproc)));

    g_initialized = true;
    console::ok("Overlay ready [INSERT to toggle]");
    return true;
}

// Called from our hooked Flip (runs on the render thread)
inline int g_init_attempts = 0;

inline bool __cdecl hooked_flip(const void* a1, const void* a2, HWND a3) {
    if (!g_initialized && g_init_attempts < 3) {
        g_init_attempts++;
        try_init();
    }

    if (g_initialized && g_d3d11_device && g_d3d11_context) {
        // If we lost the RTV (window resize etc), try to get it again
        if (!g_rtv) {
            ID3D11RenderTargetView* game_rtv = nullptr;
            g_d3d11_context->OMGetRenderTargets(1, &game_rtv, nullptr);
            if (game_rtv) {
                ID3D11Resource* res = nullptr;
                game_rtv->GetResource(&res);
                if (res) {
                    ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
                    if (tex) {
                        g_d3d11_device->CreateRenderTargetView(tex, nullptr, &g_rtv);
                        tex->Release();
                    }
                    res->Release();
                }
                game_rtv->Release();
            }
        }

        if (!g_rtv || !g_swap_chain) return g_orig_flip(a1, a2, a3);

        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_ctx);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw();

        ImGui::Render();

        float clear[] = {0, 0, 0, 0}; // transparent
        g_d3d11_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_d3d11_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swap_chain->Present(0, 0);

        ImGui::SetCurrentContext(prev);
    }

    return g_orig_flip(a1, a2, a3);
}

// Install by IAT-patching CMyBitmap::Flip
inline bool install() {
    uintptr_t base = game::get_base();

    // Flip IAT entry is at base + 0x408DA8
    uintptr_t* iat_slot = reinterpret_cast<uintptr_t*>(base + 0x408DA8);
    g_orig_flip = reinterpret_cast<Flip_t>(*iat_slot);

    if (!g_orig_flip) {
        console::error("Flip IAT entry is null");
        return false;
    }

    DWORD old = 0;
    VirtualProtect(iat_slot, sizeof(uintptr_t), PAGE_READWRITE, &old);
    *iat_slot = reinterpret_cast<uintptr_t>(hooked_flip);
    VirtualProtect(iat_slot, sizeof(uintptr_t), old, &old);

    console::info("Flip IAT hooked (orig=0x%llX)", reinterpret_cast<uintptr_t>(g_orig_flip));
    return true;
}

inline void shutdown() {
    if (g_orig_wndproc && g_hwnd)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    if (g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ctx);
    }
    if (g_rtv) g_rtv->Release();
    if (g_swap_chain) g_swap_chain->Release();
    if (g_d3d11_context) g_d3d11_context->Release();
    if (g_d3d11_device) g_d3d11_device->Release();

    // Restore IAT
    uintptr_t base = game::get_base();
    uintptr_t* iat_slot = reinterpret_cast<uintptr_t*>(base + 0x408DA8);
    if (g_orig_flip) {
        DWORD old = 0;
        VirtualProtect(iat_slot, sizeof(uintptr_t), PAGE_READWRITE, &old);
        *iat_slot = reinterpret_cast<uintptr_t>(g_orig_flip);
        VirtualProtect(iat_slot, sizeof(uintptr_t), old, &old);
    }
}

} // namespace overlay
