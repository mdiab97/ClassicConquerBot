#pragma once
#include "game.h"
#include "console.h"
#include <MinHook.h>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <cstdarg>

namespace login {

constexpr uintptr_t OFF_GAME_STATE   = 0x4D434C;
// Login object starts at qword_1404DFBD8 (vtable at [0], within singleton at 0x4DFB60+0x78)
constexpr uintptr_t OFF_LOGIN_OBJECT = 0x4DFBD8;
constexpr int LOGIN_USERNAME_OFF     = 0x48;   // char[64]
constexpr int LOGIN_PASSWORD_OFF     = 0x88;   // char[64]
constexpr int LOGIN_SERVER_OFF       = 0xC8;   // shared_ptr<Server>
constexpr uintptr_t OFF_SERVER_CTX   = 0x4DF970;
constexpr uintptr_t FN_SHARED_PTR_ASSIGN = 0x0B3390;
constexpr uintptr_t FN_LOGIN_HANDLER = 0xDF300;

inline std::atomic<bool> g_login_pending{false};
inline char g_username[64]{};
inline char g_password[64]{};
inline int g_server_index{0};
inline std::atomic<bool> g_login_done{false};

inline void flog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    FILE* f = fopen("C:\\login_v2.log", "a");
    if (f) { fprintf(f, "[%u] %s\n", GetTickCount(), buf); fflush(f); fclose(f); }
}

// ── MinHook-based Button hook ───────────────────────────────────────
// Original ImGui::Button signature: char __fastcall Button(const char*, int, ImVec2)
using Button_t = char(__fastcall*)(const char*, unsigned __int64, int);
inline Button_t g_orig_button = nullptr;
inline void* g_button_target = nullptr;

inline char __fastcall hooked_button(const char* label, unsigned __int64 a2, int a3) {
    // Detach immediately before returning
    MH_DisableHook(g_button_target);
    MH_RemoveHook(g_button_target);
    flog("Button hook fired (label='%s'), detached, returning 1", label ? label : "?");
    return 1;
}

inline void hook_button_once() {
    uintptr_t addr = game::get_base() + 0x38110;
    g_button_target = reinterpret_cast<void*>(addr);

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        flog("MH_Initialize failed");
        return;
    }
    if (MH_CreateHook(g_button_target, reinterpret_cast<void*>(&hooked_button),
            reinterpret_cast<void**>(&g_orig_button)) != MH_OK) {
        flog("MH_CreateHook failed");
        return;
    }
    if (MH_EnableHook(g_button_target) != MH_OK) {
        flog("MH_EnableHook failed");
        return;
    }
    flog("Button hook installed at 0x%llX", addr);
}

inline bool is_login_screen() {
    uintptr_t base = game::get_base();
    int32_t state = 0;
    game::safe_read<int32_t>(base + OFF_GAME_STATE, state);
    return state == 1000 || state == 1010 || state == 1100;
}

inline bool try_auto_login() {
    if (g_login_done.load()) return true;
    if (!g_login_pending.load()) return false;
    if (!is_login_screen()) return false;

    uintptr_t obj = game::get_base() + OFF_LOGIN_OBJECT;

    // Verify vtable is set (object is initialized)
    uintptr_t vt = 0;
    game::safe_read(obj, vt);
    if (!vt || !game::is_valid_ptr(vt)) {
        flog("login object not initialized (vt=0x%llX)", vt);
        return false;
    }

    flog("login object at 0x%llX, vtable=0x%llX", obj, vt);

    // Read current username to check if already set
    char cur_user[17]{};
    __try { memcpy(cur_user, reinterpret_cast<void*>(obj + LOGIN_USERNAME_OFF), 16); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    flog("current username buffer: '%s'", cur_user);

    // Write username
    __try {
        char* buf = reinterpret_cast<char*>(obj + LOGIN_USERNAME_OFF);
        memset(buf, 0, 64);
        strncpy(buf, g_username, 63);
        flog("username written to 0x%llX: '%s'", obj + LOGIN_USERNAME_OFF, g_username);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        flog("username write CRASHED");
        return false;
    }

    // Write password
    __try {
        char* buf = reinterpret_cast<char*>(obj + LOGIN_PASSWORD_OFF);
        memset(buf, 0, 64);
        strncpy(buf, g_password, 63);
        flog("password written to 0x%llX", obj + LOGIN_PASSWORD_OFF);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        flog("password write CRASHED");
        return false;
    }

    // Verify username was written
    char verify[17]{};
    __try { memcpy(verify, reinterpret_cast<void*>(obj + LOGIN_USERNAME_OFF), 16); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    flog("verification read: '%s'", verify);

    // Select server by manually copying shared_ptr (no function calls)
    uintptr_t base = game::get_base();
    uintptr_t list_begin = 0, list_end = 0;
    game::safe_read(base + OFF_SERVER_CTX + 0x40, list_begin);
    game::safe_read(base + OFF_SERVER_CTX + 0x48, list_end);
    int count = (list_begin && game::is_valid_ptr(list_begin) && list_end > list_begin) ?
        static_cast<int>((list_end - list_begin) / 16) : 0;
    flog("server list: %d servers", count);

    if (count > 0) {
        int idx = (g_server_index >= 0 && g_server_index < count) ? g_server_index : 0;
        uintptr_t src_addr = list_begin + idx * 16;

        // Read source shared_ptr: [ptr, control_block]
        uintptr_t src_ptr = 0, src_cb = 0;
        game::safe_read(src_addr, src_ptr);
        game::safe_read(src_addr + 8, src_cb);
        flog("server %d: ptr=0x%llX cb=0x%llX", idx, src_ptr, src_cb);

        if (src_ptr && game::is_valid_ptr(src_ptr)) {
            // Increment refcount atomically (control_block+8 is the strong refcount)
            if (src_cb && game::is_valid_ptr(src_cb)) {
                InterlockedIncrement(reinterpret_cast<volatile long*>(src_cb + 8));
            }
            // Write to destination (obj+0xC8 and obj+0xD0)
            // Only safe if old value is null (no release needed)
            uintptr_t old_ptr = 0;
            game::safe_read(obj + LOGIN_SERVER_OFF, old_ptr);
            if (old_ptr == 0) {
                game::safe_write(obj + LOGIN_SERVER_OFF, src_ptr);
                game::safe_write(obj + LOGIN_SERVER_OFF + 8, src_cb);
                flog("server %d set (manual copy)", idx);
            } else {
                flog("server already set (old=0x%llX), skipping", old_ptr);
            }
        }
    }

    uintptr_t srv = 0;
    game::safe_read(obj + LOGIN_SERVER_OFF, srv);
    flog("final server ptr: 0x%llX", srv);

    // Hook ImGui::Button via MinHook - return 1 once then detach
    flog("installing Button hook at base+0x38110");
    hook_button_once();

    console::ok("[LOGIN] Credentials + server set, Button hook installed");
    g_login_done.store(true);
    return true;
}

inline void queue_login(const char* username, const char* password, int server_idx) {
    strncpy(g_username, username, 63);
    g_username[63] = 0;
    strncpy(g_password, password, 63);
    g_password[63] = 0;
    g_server_index = server_idx;
    g_login_done.store(false);
    g_login_pending.store(true);
}

} // namespace login
