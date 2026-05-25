#include "version_proxy.h"
#include <cstdio>

// ── Real function pointers ──────────────────────────────────────────
static HMODULE s_real_dll = nullptr;

#define PROXY_FUNC(name) static FARPROC s_##name = nullptr;
PROXY_FUNC(GetFileVersionInfoA)
PROXY_FUNC(GetFileVersionInfoByHandle)
PROXY_FUNC(GetFileVersionInfoExA)
PROXY_FUNC(GetFileVersionInfoExW)
PROXY_FUNC(GetFileVersionInfoSizeA)
PROXY_FUNC(GetFileVersionInfoSizeExA)
PROXY_FUNC(GetFileVersionInfoSizeExW)
PROXY_FUNC(GetFileVersionInfoSizeW)
PROXY_FUNC(GetFileVersionInfoW)
PROXY_FUNC(VerFindFileA)
PROXY_FUNC(VerFindFileW)
PROXY_FUNC(VerInstallFileA)
PROXY_FUNC(VerInstallFileW)
PROXY_FUNC(VerQueryValueA)
PROXY_FUNC(VerQueryValueW)
#undef PROXY_FUNC

namespace proxy {

bool init() {
    // Build path to the REAL version.dll in System32
    char sys_dir[MAX_PATH]{};
    GetSystemDirectoryA(sys_dir, MAX_PATH);
    char real_path[MAX_PATH]{};
    snprintf(real_path, MAX_PATH, "%s\\version.dll", sys_dir);

    s_real_dll = LoadLibraryA(real_path);
    if (!s_real_dll) return false;

#define RESOLVE(name) s_##name = GetProcAddress(s_real_dll, #name)
    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoByHandle);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);
#undef RESOLVE

    return true;
}

void shutdown() {
    if (s_real_dll) {
        FreeLibrary(s_real_dll);
        s_real_dll = nullptr;
    }
}

} // namespace proxy

// ── Forwarded exports ───────────────────────────────────────────────
// Each export calls through to the real version.dll function.

#define PROXY_EXPORT(name) \
    extern "C" __declspec(dllexport) LONG_PTR WINAPI name() { \
        if (!s_##name) return 0; \
        return reinterpret_cast<LONG_PTR(WINAPI*)()>(s_##name)(); \
    }

// We use naked asm forwarding for proper argument passthrough on x64.
// MSVC x64 doesn't support __declspec(naked), so we use JMP thunks via .asm
// or the simpler approach: .def file + linker /EXPORT with forwarding.
//
// Simplest correct approach: define exports that do a tail-call JMP.
// On x64 with __fastcall, we just need to JMP to the real function
// since all args are already in the right registers/stack positions.

extern "C" {

__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoA() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoA)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoByHandle() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoByHandle)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoExA() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoExA)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoExW() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoExW)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoSizeA() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoSizeA)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoSizeExA() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoSizeExA)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoSizeExW() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoSizeExW)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoSizeW() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoSizeW)();
}
__declspec(dllexport) void WINAPI proxy_GetFileVersionInfoW() {
    reinterpret_cast<void(WINAPI*)()>(s_GetFileVersionInfoW)();
}
__declspec(dllexport) void WINAPI proxy_VerFindFileA() {
    reinterpret_cast<void(WINAPI*)()>(s_VerFindFileA)();
}
__declspec(dllexport) void WINAPI proxy_VerFindFileW() {
    reinterpret_cast<void(WINAPI*)()>(s_VerFindFileW)();
}
__declspec(dllexport) void WINAPI proxy_VerInstallFileA() {
    reinterpret_cast<void(WINAPI*)()>(s_VerInstallFileA)();
}
__declspec(dllexport) void WINAPI proxy_VerInstallFileW() {
    reinterpret_cast<void(WINAPI*)()>(s_VerInstallFileW)();
}
__declspec(dllexport) void WINAPI proxy_VerQueryValueA() {
    reinterpret_cast<void(WINAPI*)()>(s_VerQueryValueA)();
}
__declspec(dllexport) void WINAPI proxy_VerQueryValueW() {
    reinterpret_cast<void(WINAPI*)()>(s_VerQueryValueW)();
}

} // extern "C"
