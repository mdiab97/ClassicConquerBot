#pragma once
#include <cstdint>
#include <string>

namespace ccbot::spoof {

struct SpoofConfig {
    std::string username{"DESKTOP-RNDUSER"};
    uint32_t volume_serial{0};     // 0 = generate random
    bool spoof_username{true};
    bool spoof_volume{true};
    bool spoof_sysinfo{true};
    bool spoof_network{true};
};

// Initialize hooks - call once from DllMain
bool init(const SpoofConfig& config = {});

// Cleanup hooks
void shutdown();

// Get the generated/spoofed values for reporting back to GUI
std::string get_spoofed_username();
uint32_t get_spoofed_volume_serial();

} // namespace ccbot::spoof
