#include "app.h"
#include <Windows.h>
#include "imgui.h"

// Forward declare
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static App g_app;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            g_app.resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"CCBotGUI";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"CQ Graphics Configuration",
        WS_OVERLAPPEDWINDOW,
        50, 50, 1280, 820,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_app.init(hwnd)) {
        g_app.shutdown();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Crash handler
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "CRASH at 0x%llX\nCode: 0x%08X\nFlags: 0x%08X\n\nThis info has been copied to clipboard.",
            (unsigned long long)ep->ExceptionRecord->ExceptionAddress,
            (unsigned int)ep->ExceptionRecord->ExceptionCode,
            (unsigned int)ep->ExceptionRecord->ExceptionFlags);

        // Copy to clipboard
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(buf) + 1);
            if (h) {
                memcpy(GlobalLock(h), buf, strlen(buf) + 1);
                GlobalUnlock(h);
                SetClipboardData(CF_TEXT, h);
            }
            CloseClipboard();
        }

        // Write to file with module base for offset calculation
        HMODULE hMod = GetModuleHandleA(nullptr);
        FILE* f = nullptr;
        fopen_s(&f, "crash.log", "a");
        if (f) {
            auto base = (unsigned long long)hMod;
            auto rip = (unsigned long long)ep->ContextRecord->Rip;
            fprintf(f, "--- CRASH ---\n%s\n"
                "Module base: 0x%llX\n"
                "Crash offset: 0x%llX (base + 0x%llX)\n"
                "RAX=0x%llX RBX=0x%llX RCX=0x%llX RDX=0x%llX\n"
                "RSI=0x%llX RDI=0x%llX RSP=0x%llX RBP=0x%llX\n"
                "R8=0x%llX R9=0x%llX R10=0x%llX R11=0x%llX\n"
                "RIP=0x%llX\n\n",
                buf, base, rip, rip - base,
                (unsigned long long)ep->ContextRecord->Rax,
                (unsigned long long)ep->ContextRecord->Rbx,
                (unsigned long long)ep->ContextRecord->Rcx,
                (unsigned long long)ep->ContextRecord->Rdx,
                (unsigned long long)ep->ContextRecord->Rsi,
                (unsigned long long)ep->ContextRecord->Rdi,
                (unsigned long long)ep->ContextRecord->Rsp,
                (unsigned long long)ep->ContextRecord->Rbp,
                (unsigned long long)ep->ContextRecord->R8,
                (unsigned long long)ep->ContextRecord->R9,
                (unsigned long long)ep->ContextRecord->R10,
                (unsigned long long)ep->ContextRecord->R11,
                rip);
            fclose(f);
        }

        MessageBoxA(nullptr, buf, "CQGraphicsConfig Crash", MB_OK | MB_ICONERROR);
        return EXCEPTION_EXECUTE_HANDLER;
    });

    // Main loop
    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        g_app.render_frame();
    }

    g_app.shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
