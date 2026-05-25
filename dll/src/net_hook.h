#pragma once
// ═══════════════════════════════════════════════════════════════════════
// NET HOOK — Redirect game server connection through GUI proxy
// ═══════════════════════════════════════════════════════════════════════
//
// Hooks connect() to redirect the game's outbound server connection to
// localhost:PROXY_PORT. The GUI reads servers.json for the real address.
//

#include <Windows.h>
#include <WinSock2.h>
#include <MinHook.h>
#include <cstdint>
#include <atomic>
#include "console.h"

#pragma comment(lib, "ws2_32.lib")

namespace net_hook {

inline std::atomic<uint16_t> g_proxy_port{1412};
inline uint16_t g_login_port{9959};
inline std::atomic<bool> g_installed{false};
inline std::atomic<bool> g_redirect_enabled{true}; // set false to bypass proxy

using connect_t = int(WINAPI*)(SOCKET, const sockaddr*, int);
inline connect_t orig_connect = nullptr;

inline int WINAPI hooked_connect(SOCKET s, const sockaddr* name, int namelen) {
    if (name && name->sa_family == AF_INET && namelen >= sizeof(sockaddr_in)) {
        auto* addr = reinterpret_cast<const sockaddr_in*>(name);
        uint32_t ip = ntohl(addr->sin_addr.s_addr);
        uint16_t port = ntohs(addr->sin_port);

        // Only redirect connections to the login server port (when enabled)
        if (g_redirect_enabled.load() && ip != 0x7F000001 && port == g_login_port) {
            uint16_t pp = g_proxy_port.load();
            console::ok("[NET] Redirecting %u.%u.%u.%u:%u -> localhost:%u",
                (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF, port, pp);

            sockaddr_in proxy_addr{};
            proxy_addr.sin_family = AF_INET;
            proxy_addr.sin_port = htons(pp);
            proxy_addr.sin_addr.s_addr = htonl(0x7F000001);

            return orig_connect(s, reinterpret_cast<const sockaddr*>(&proxy_addr), sizeof(proxy_addr));
        }
    }
    return orig_connect(s, name, namelen);
}

inline bool install() {
    if (g_installed.load()) return true;

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2) { console::info("[NET] ws2_32.dll not found"); return false; }

    auto fn_connect = reinterpret_cast<void*>(GetProcAddress(ws2, "connect"));
    if (!fn_connect) { console::info("[NET] connect not found"); return false; }

    MH_Initialize();

    if (MH_CreateHook(fn_connect, reinterpret_cast<void*>(&hooked_connect),
            reinterpret_cast<void**>(&orig_connect)) != MH_OK) {
        console::info("[NET] Failed to hook connect");
        return false;
    }

    MH_EnableHook(fn_connect);
    g_installed.store(true);
    console::ok("[NET] Connect hook installed (redirect to proxy:%u)", g_proxy_port.load());
    return true;
}

inline void uninstall() {
    if (!g_installed.load()) return;
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (ws2) {
        auto fn = reinterpret_cast<void*>(GetProcAddress(ws2, "connect"));
        if (fn) MH_DisableHook(fn);
    }
    g_installed.store(false);
}

} // namespace net_hook
