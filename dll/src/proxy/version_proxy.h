#pragma once
#include <Windows.h>

namespace proxy {

// Load the real version.dll from System32 and resolve all exports.
// Must be called once at DLL_PROCESS_ATTACH.
bool init();
void shutdown();

} // namespace proxy
