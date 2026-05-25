#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace ccbot {

// ── Process utilities ───────────────────────────────────────────────

struct ProcessInfo {
    DWORD pid{0};
    std::string name;
};

inline std::vector<ProcessInfo> find_processes(const std::string& name_filter) {
    std::vector<ProcessInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            char narrow[260]{};
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, narrow, sizeof(narrow), nullptr, nullptr);
            if (name_filter.empty() || _stricmp(narrow, name_filter.c_str()) == 0) {
                result.push_back({pe.th32ProcessID, narrow});
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

// ── Manual Map Injector ─────────────────────────────────────────────

using LogFn = std::function<void(const char*)>;

using f_LoadLibraryA = HINSTANCE(WINAPI*)(const char*);
using f_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using f_DLL_ENTRY_POINT = BOOL(WINAPI*)(void*, DWORD, void*);
using f_RtlAddFunctionTable = BOOL(WINAPIV*)(PRUNTIME_FUNCTION, DWORD, DWORD64);

struct ManualMapData {
    f_LoadLibraryA pLoadLibraryA;
    f_GetProcAddress pGetProcAddress;
    f_RtlAddFunctionTable pRtlAddFunctionTable;
    BYTE* pbase;
    HINSTANCE hMod;
    DWORD fdwReasonParam;
    LPVOID reservedParam;
    BOOL SEHSupport;
};

// The shellcode that runs inside the target process.
// Must be compiled with optimizations off and no runtime checks.
#pragma runtime_checks("", off)
#pragma optimize("", off)
inline void __stdcall MapShellcode(ManualMapData* pData) {
    if (!pData) return;

    BYTE* pBase = pData->pbase;
    auto* pOpt = &reinterpret_cast<IMAGE_NT_HEADERS*>(
        pBase + reinterpret_cast<IMAGE_DOS_HEADER*>(pBase)->e_lfanew)->OptionalHeader;

    auto _LoadLibraryA = pData->pLoadLibraryA;
    auto _GetProcAddress = pData->pGetProcAddress;
    auto _RtlAddFunctionTable = pData->pRtlAddFunctionTable;
    auto _DllMain = reinterpret_cast<f_DLL_ENTRY_POINT>(pBase + pOpt->AddressOfEntryPoint);

    // Base relocations
    BYTE* LocationDelta = pBase - pOpt->ImageBase;
    if (LocationDelta) {
        if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
            auto* pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
            const auto* pRelocEnd = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                reinterpret_cast<uintptr_t>(pRelocData) +
                pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);

            while (pRelocData < pRelocEnd && pRelocData->SizeOfBlock) {
                UINT count = (pRelocData->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* pRelInfo = reinterpret_cast<WORD*>(pRelocData + 1);
                for (UINT i = 0; i != count; ++i, ++pRelInfo) {
                    if ((*pRelInfo >> 0x0C) == IMAGE_REL_BASED_DIR64) {
                        auto* pPatch = reinterpret_cast<UINT_PTR*>(
                            pBase + pRelocData->VirtualAddress + ((*pRelInfo) & 0xFFF));
                        *pPatch += reinterpret_cast<UINT_PTR>(LocationDelta);
                    }
                }
                pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<BYTE*>(pRelocData) + pRelocData->SizeOfBlock);
            }
        }
    }

    // Resolve imports
    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto* pImport = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        while (pImport->Name) {
            char* szMod = reinterpret_cast<char*>(pBase + pImport->Name);
            HINSTANCE hDll = _LoadLibraryA(szMod);
            ULONG_PTR* pThunk = reinterpret_cast<ULONG_PTR*>(
                pBase + (pImport->OriginalFirstThunk ? pImport->OriginalFirstThunk : pImport->FirstThunk));
            ULONG_PTR* pFunc = reinterpret_cast<ULONG_PTR*>(pBase + pImport->FirstThunk);
            for (; *pThunk; ++pThunk, ++pFunc) {
                if (IMAGE_SNAP_BY_ORDINAL(*pThunk)) {
                    *pFunc = reinterpret_cast<ULONG_PTR>(
                        _GetProcAddress(hDll, reinterpret_cast<char*>(*pThunk & 0xFFFF)));
                } else {
                    auto* pName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(pBase + *pThunk);
                    *pFunc = reinterpret_cast<ULONG_PTR>(_GetProcAddress(hDll, pName->Name));
                }
            }
            ++pImport;
        }
    }

    // TLS callbacks
    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* pTLS = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        auto* pCallback = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(pTLS->AddressOfCallBacks);
        for (; pCallback && *pCallback; ++pCallback)
            (*pCallback)(pBase, DLL_PROCESS_ATTACH, nullptr);
    }

    // Exception handling (x64 SEH)
    bool seh_failed = false;
    if (pData->SEHSupport) {
        auto excep = pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (excep.Size) {
            if (!_RtlAddFunctionTable(
                    reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(pBase + excep.VirtualAddress),
                    excep.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY),
                    reinterpret_cast<DWORD64>(pBase))) {
                seh_failed = true;
            }
        }
    }

    // Call DllMain
    _DllMain(pBase, pData->fdwReasonParam, pData->reservedParam);

    pData->hMod = seh_failed
        ? reinterpret_cast<HINSTANCE>(0x505050)
        : reinterpret_cast<HINSTANCE>(pBase);
}
#pragma optimize("", on)
#pragma runtime_checks("", restore)

// Inject a DLL into the target process via manual mapping.
// dll_path: path to the DLL file on disk
// pid: target process ID
// log: optional logging callback
inline bool inject_manual_map(const std::string& dll_path, DWORD pid, LogFn log = {}) {
    auto say = [&](const char* fmt, ...) {
        if (!log) return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log(buf);
    };

    // Read DLL file
    HANDLE hFile = CreateFileA(dll_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        say("Failed to open DLL: %s", dll_path.c_str());
        return false;
    }
    DWORD file_size = GetFileSize(hFile, nullptr);
    std::vector<BYTE> dll_data(file_size);
    DWORD read = 0;
    ReadFile(hFile, dll_data.data(), file_size, &read, nullptr);
    CloseHandle(hFile);

    if (read < sizeof(IMAGE_DOS_HEADER) ||
        reinterpret_cast<IMAGE_DOS_HEADER*>(dll_data.data())->e_magic != IMAGE_DOS_SIGNATURE) {
        say("Invalid DLL file (bad MZ header)");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        dll_data.data() + reinterpret_cast<IMAGE_DOS_HEADER*>(dll_data.data())->e_lfanew);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        say("DLL is not x64");
        return false;
    }

    // Open target process
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        say("Failed to open process %u (error %u)", pid, GetLastError());
        return false;
    }

    auto cleanup = [&](BYTE* base = nullptr, BYTE* data_alloc = nullptr, void* sc = nullptr) {
        if (base) VirtualFreeEx(hProc, base, 0, MEM_RELEASE);
        if (data_alloc) VirtualFreeEx(hProc, data_alloc, 0, MEM_RELEASE);
        if (sc) { VirtualFreeEx(hProc, sc, 0, MEM_RELEASE); }
        CloseHandle(hProc);
    };

    // Allocate image in target
    auto* pOpt = &nt->OptionalHeader;
    BYTE* pTarget = static_cast<BYTE*>(
        VirtualAllocEx(hProc, nullptr, pOpt->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!pTarget) {
        say("VirtualAllocEx failed (%u)", GetLastError());
        cleanup();
        return false;
    }
    say("Allocated 0x%X bytes at %p in target", pOpt->SizeOfImage, pTarget);

    // Write PE header
    WriteProcessMemory(hProc, pTarget, dll_data.data(), 0x1000, nullptr);

    // Write sections
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->SizeOfRawData) {
            WriteProcessMemory(hProc, pTarget + sec->VirtualAddress,
                               dll_data.data() + sec->PointerToRawData,
                               sec->SizeOfRawData, nullptr);
        }
    }

    // Prepare mapping data
    ManualMapData mmd{};
    mmd.pLoadLibraryA = LoadLibraryA;
    mmd.pGetProcAddress = GetProcAddress;
    mmd.pRtlAddFunctionTable = reinterpret_cast<f_RtlAddFunctionTable>(RtlAddFunctionTable);
    mmd.pbase = pTarget;
    mmd.fdwReasonParam = DLL_PROCESS_ATTACH;
    mmd.reservedParam = nullptr;
    mmd.SEHSupport = TRUE;

    auto* pMmd = static_cast<BYTE*>(
        VirtualAllocEx(hProc, nullptr, sizeof(mmd), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pMmd) { say("Alloc MMD failed"); cleanup(pTarget); return false; }
    WriteProcessMemory(hProc, pMmd, &mmd, sizeof(mmd), nullptr);

    // Write shellcode
    void* pShell = VirtualAllocEx(hProc, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pShell) { say("Alloc shellcode failed"); cleanup(pTarget, pMmd); return false; }
    WriteProcessMemory(hProc, pShell, reinterpret_cast<void*>(MapShellcode), 0x1000, nullptr);

    // Execute
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pShell), pMmd, 0, nullptr);
    if (!hThread) {
        say("CreateRemoteThread failed (%u)", GetLastError());
        cleanup(pTarget, pMmd, pShell);
        return false;
    }
    say("Remote thread created, waiting...");

    // Wait for shellcode to finish
    HINSTANCE hCheck = nullptr;
    for (int i = 0; i < 500 && !hCheck; ++i) {
        DWORD exitcode = 0;
        GetExitCodeProcess(hProc, &exitcode);
        if (exitcode != STILL_ACTIVE) {
            say("Target process crashed (exit %u)", exitcode);
            CloseHandle(hThread);
            cleanup(nullptr, pMmd, pShell);
            return false;
        }
        ManualMapData check{};
        ReadProcessMemory(hProc, pMmd, &check, sizeof(check), nullptr);
        hCheck = check.hMod;
        Sleep(10);
    }

    CloseHandle(hThread);

    if (!hCheck) {
        say("Injection timed out");
        cleanup(pTarget, pMmd, pShell);
        return false;
    }
    if (hCheck == reinterpret_cast<HINSTANCE>(0x505050)) {
        say("Warning: SEH registration failed (DLL still loaded)");
    }

    say("DLL mapped at %p", hCheck);

    // Cleanup: clear header, free shellcode + mapping data
    std::vector<BYTE> zeros(0x1000, 0);
    WriteProcessMemory(hProc, pTarget, zeros.data(), 0x1000, nullptr);
    VirtualFreeEx(hProc, pShell, 0, MEM_RELEASE);
    VirtualFreeEx(hProc, pMmd, 0, MEM_RELEASE);

    // Adjust section protections
    sec = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Misc.VirtualSize) {
            DWORD prot = PAGE_READONLY;
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) prot = PAGE_EXECUTE_READ;
            if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_READWRITE;
            DWORD old = 0;
            VirtualProtectEx(hProc, pTarget + sec->VirtualAddress, sec->Misc.VirtualSize, prot, &old);
        }
    }

    CloseHandle(hProc);
    say("Injection complete");
    return true;
}

} // namespace ccbot
