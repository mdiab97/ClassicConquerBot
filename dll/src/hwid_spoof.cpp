#include "hwid_spoof.h"
#include "console.h"
#include <Windows.h>
#include <WinSock2.h>
#include <iphlpapi.h>
#include <random>
#include <string>
#include <cstring>
#include <DbgHelp.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dbghelp.lib")

namespace ccbot::spoof {

// ── Spoofed state ───────────────────────────────────────────────────
static SpoofConfig s_config;
static std::string s_spoofed_username;
static uint32_t s_spoofed_volume_serial{0};

static uint32_t random_u32() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_int_distribution<uint32_t>{}(rng);
}

static std::string random_desktop_name() {
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::mt19937 rng{std::random_device{}()};
    std::string name = "DESKTOP-";
    for (int i = 0; i < 7; ++i)
        name += charset[std::uniform_int_distribution<int>{0, sizeof(charset) - 2}(rng)];
    return name;
}

// ── Original function pointers (saved before patching) ──────────────
using GetUserNameA_t = BOOL(WINAPI*)(LPSTR, LPDWORD);
using GetVolumeInformationA_t = BOOL(WINAPI*)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD);
using GetNativeSystemInfo_t = void(WINAPI*)(LPSYSTEM_INFO);
using GetComputerNameA_t = BOOL(WINAPI*)(LPSTR, LPDWORD);

static GetUserNameA_t orig_GetUserNameA = nullptr;
static GetVolumeInformationA_t orig_GetVolumeInformationA = nullptr;
static GetNativeSystemInfo_t orig_GetNativeSystemInfo = nullptr;
static GetComputerNameA_t orig_GetComputerNameA = nullptr;

// ── Hook implementations ────────────────────────────────────────────

static BOOL WINAPI hook_GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer) {
    if (!s_config.spoof_username || !orig_GetUserNameA)
        return orig_GetUserNameA ? orig_GetUserNameA(lpBuffer, pcbBuffer) : FALSE;

    // Get what the real value would be for logging
    char real_buf[256]{}; DWORD real_sz = sizeof(real_buf);
    orig_GetUserNameA(real_buf, &real_sz);
    console::info("[HWID] GetUserNameA called: real=\"%s\" -> spoofed=\"%s\"", real_buf, s_spoofed_username.c_str());

    DWORD needed = static_cast<DWORD>(s_spoofed_username.size() + 1);
    if (*pcbBuffer < needed) {
        *pcbBuffer = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(lpBuffer, s_spoofed_username.c_str(), needed);
    *pcbBuffer = needed;
    return TRUE;
}

static BOOL WINAPI hook_GetVolumeInformationA(
    LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
    LPDWORD lpVolumeSerialNumber, LPDWORD lpMaxComponentLength,
    LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize)
{
    if (!orig_GetVolumeInformationA) return FALSE;
    BOOL result = orig_GetVolumeInformationA(
        lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize,
        lpVolumeSerialNumber, lpMaxComponentLength,
        lpFileSystemFlags, lpFileSystemNameBuffer, nFileSystemNameSize);
    if (result && lpVolumeSerialNumber) {
        DWORD real_serial = *lpVolumeSerialNumber;
        if (s_config.spoof_volume)
            *lpVolumeSerialNumber = s_spoofed_volume_serial;
        console::info("[HWID] GetVolumeInformationA(\"%s\"): real=0x%08X -> spoofed=0x%08X",
                      lpRootPathName ? lpRootPathName : "null", real_serial, *lpVolumeSerialNumber);
    }
    return result;
}

static void WINAPI hook_GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
    if (!orig_GetNativeSystemInfo) return;
    orig_GetNativeSystemInfo(lpSystemInfo);
    DWORD real_oem = lpSystemInfo->dwOemId;
    if (s_config.spoof_sysinfo)
        lpSystemInfo->dwOemId = s_spoofed_volume_serial & 0xFFFF;
    console::info("[HWID] GetNativeSystemInfo: OemId real=0x%X -> spoofed=0x%X, Arch=%u, Cores=%u",
                  real_oem, lpSystemInfo->dwOemId, lpSystemInfo->wProcessorArchitecture, lpSystemInfo->dwNumberOfProcessors);
}

static BOOL WINAPI hook_GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize) {
    if (!s_config.spoof_username || !orig_GetComputerNameA)
        return orig_GetComputerNameA ? orig_GetComputerNameA(lpBuffer, nSize) : FALSE;

    char real_buf[256]{}; DWORD real_sz = sizeof(real_buf);
    orig_GetComputerNameA(real_buf, &real_sz);
    console::info("[HWID] GetComputerNameA called: real=\"%s\" -> spoofed=\"%s\"", real_buf, s_spoofed_username.c_str());

    DWORD needed = static_cast<DWORD>(s_spoofed_username.size() + 1);
    if (*nSize < needed) {
        *nSize = needed;
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    memcpy(lpBuffer, s_spoofed_username.c_str(), needed);
    *nSize = static_cast<DWORD>(s_spoofed_username.size());
    return TRUE;
}

// ── IAT Patching ────────────────────────────────────────────────────
// Patches the Import Address Table of the main exe module.
// Only changes data pointers - no code bytes are modified.

struct IatEntry {
    void** slot;       // address of the IAT slot in the exe
    void* original;    // original function pointer (to restore later)
    void* hook;        // our replacement
};

static IatEntry s_iat_patches[4]{};
static int s_iat_count = 0;

static bool patch_iat(HMODULE module, const char* dll_name, const char* func_name,
                      void* hook_func, void** out_original)
{
    ULONG size = 0;
    auto* imports = static_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        ImageDirectoryEntryToDataEx(module, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size, nullptr));
    if (!imports) return false;

    for (; imports->Name; ++imports) {
        auto* mod_name = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(module) + imports->Name);
        if (_stricmp(mod_name, dll_name) != 0) continue;

        auto* orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + imports->OriginalFirstThunk);
        auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + imports->FirstThunk);

        for (; orig_thunk->u1.AddressOfData; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) continue;

            auto* import_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<BYTE*>(module) + orig_thunk->u1.AddressOfData);

            if (strcmp(import_name->Name, func_name) != 0) continue;

            // Found the IAT entry - patch it
            *out_original = reinterpret_cast<void*>(iat_thunk->u1.Function);

            DWORD old_protect = 0;
            VirtualProtect(&iat_thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protect);
            iat_thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook_func);
            VirtualProtect(&iat_thunk->u1.Function, sizeof(void*), old_protect, &old_protect);

            // Save for cleanup
            if (s_iat_count < 4) {
                s_iat_patches[s_iat_count++] = {
                    reinterpret_cast<void**>(&iat_thunk->u1.Function),
                    *out_original,
                    hook_func
                };
            }
            return true;
        }
    }
    return false;
}

// ── Public API ──────────────────────────────────────────────────────

bool init(const SpoofConfig& config) {
    s_config = config;
    s_spoofed_username = config.username.empty() ? random_desktop_name() : config.username;
    s_spoofed_volume_serial = config.volume_serial ? config.volume_serial : random_u32();
    s_iat_count = 0;

    HMODULE exe = GetModuleHandleA(nullptr);

    patch_iat(exe, "advapi32.dll", "GetUserNameA",
              reinterpret_cast<void*>(hook_GetUserNameA),
              reinterpret_cast<void**>(&orig_GetUserNameA));

    patch_iat(exe, "kernel32.dll", "GetVolumeInformationA",
              reinterpret_cast<void*>(hook_GetVolumeInformationA),
              reinterpret_cast<void**>(&orig_GetVolumeInformationA));

    patch_iat(exe, "kernel32.dll", "GetNativeSystemInfo",
              reinterpret_cast<void*>(hook_GetNativeSystemInfo),
              reinterpret_cast<void**>(&orig_GetNativeSystemInfo));

    patch_iat(exe, "kernel32.dll", "GetComputerNameA",
              reinterpret_cast<void*>(hook_GetComputerNameA),
              reinterpret_cast<void**>(&orig_GetComputerNameA));

    return s_iat_count > 0;
}

void shutdown() {
    // Restore original IAT entries
    for (int i = 0; i < s_iat_count; ++i) {
        DWORD old_protect = 0;
        VirtualProtect(s_iat_patches[i].slot, sizeof(void*), PAGE_READWRITE, &old_protect);
        *s_iat_patches[i].slot = s_iat_patches[i].original;
        VirtualProtect(s_iat_patches[i].slot, sizeof(void*), old_protect, &old_protect);
    }
    s_iat_count = 0;
}

std::string get_spoofed_username() { return s_spoofed_username; }
uint32_t get_spoofed_volume_serial() { return s_spoofed_volume_serial; }

} // namespace ccbot::spoof
