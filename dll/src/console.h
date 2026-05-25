#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>

namespace console {

inline void create(const char* title) {
    AllocConsole();
    SetConsoleTitleA(title);
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    // Enable ANSI escape codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

inline void destroy() {
    FreeConsole();
}

inline void log(const char* prefix, const char* color, const char* fmt, va_list args) {
    printf("%s[%s]\033[0m ", color, prefix);
    vprintf(fmt, args);
    printf("\n");
}

inline void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log("INFO", "\033[36m", fmt, args);
    va_end(args);
}

inline void ok(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log(" OK ", "\033[32m", fmt, args);
    va_end(args);
}

inline void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log("WARN", "\033[33m", fmt, args);
    va_end(args);
}

inline void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log("ERR!", "\033[31m", fmt, args);
    va_end(args);
}

} // namespace console
