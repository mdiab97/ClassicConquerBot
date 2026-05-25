#pragma once
#include "game.h"
#include "console.h"
#include <string>
#include <vector>

// Runtime field scanner - reads hero fields and reports to console
// The user verifies each value against what they see in-game.

namespace scanner {

struct FieldDef {
    const char* name;
    int offset;
    int size;      // 1, 2, 4, 8
    const char* desc;
};

// Confirmed fields (working)
// posGame.x = hero+0xD8, posGame.y = hero+0xDC

// Old offsets to verify - many may still be correct based on init analysis
static const FieldDef FIELDS[] = {
    // === CRoleInfo fields (position area - CONFIRMED) ===
    {"posGame.x",          0xD8,  4, "Cell X position (walk around to verify)"},
    {"posGame.y",          0xDC,  4, "Cell Y position"},
    {"posWorld.x",         0xE0,  4, "World pixel X"},
    {"posWorld.y",         0xE4,  4, "World pixel Y"},
    {"posScr.x",           0xE8,  4, "Screen X"},
    {"posScr.y",           0xEC,  4, "Screen Y"},

    // === Identity fields (before posGame) ===
    {"m_Info.id",          0x68,  4, "Character ID (should be >= 1000000)"},
    {"m_Info.dwTransform", 0x6C,  4, "Transform ID (0 if not transformed)"},
    {"m_Info.szName",      0x94,  0, "Character name (char[16] at 0x94, CONFIRMED from hex dump)"},

    // === Status flags ===
    {"m_nStatusFlag",      0x30,  8, "Status bitmask (bit5=dead, bit2=invis, etc)"},

    // === Type field (from init: hero+680 = 6 = MAP_HERO) ===
    {"m_nType",            0x2A8, 4, "Object type (should be 6 for hero)"},

    // === Old offsets that appear in init function ===
    {"pkMode",             0xD70, 4, "PK mode (old 860*4=3440, but 0xD70=3440)"},
    {"stamina",            0x6E0, 4, "Stamina (0-100 range)"},
    {"maxStamina",         0x6E4, 4, "Max stamina (usually 100 or 150)"},
    {"m_Silver",          0x0A30, 8, "Silver/money (CONFIRMED at 0xA30)"},
    {"npcActive",         0x1000, 4, "NPC dialog active (1 when talking to NPC)"},
    {"activeNpcId",       0x3724, 4, "Active NPC ID (non-zero when dialog open, from init: 14116-should be same)"},
    {"isVip",             0x3740, 4, "VIP status (1 if VIP)"},
};

static const int FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

// Read an std::string at a given offset (MSVC layout: buf/ptr + size + capacity)
inline std::string read_std_string(uintptr_t addr) {
    // MSVC std::string layout (x64):
    // [0..15] = SSO buffer OR [0..7] = heap pointer
    // [16] = size (uint64)
    // [24] = capacity (uint64)
    // If capacity <= 15, string is in SSO buffer. Otherwise, [0..7] is a pointer.
    uint64_t size = 0, capacity = 0;
    if (!game::safe_read<uint64_t>(addr + 16, size)) return "<read_err>";
    if (!game::safe_read<uint64_t>(addr + 24, capacity)) return "<read_err>";
    if (size == 0 || size > 1000) return "";

    const char* data = nullptr;
    if (capacity <= 15) {
        // SSO - data is inline
        data = reinterpret_cast<const char*>(addr);
    } else {
        // Heap allocated - first 8 bytes is pointer
        uintptr_t ptr = 0;
        if (!game::safe_read<uintptr_t>(addr, ptr) || !game::is_valid_ptr(ptr)) return "<bad_ptr>";
        data = reinterpret_cast<const char*>(ptr);
    }

    char buf[256]{};
    size_t copy_len = size < 255 ? size : 255;
    memcpy(buf, data, copy_len);
    return std::string(buf, copy_len);
}

inline void scan_all() {
    uintptr_t hero = game::get_hero();
    if (!hero) {
        console::error("[SCAN] Hero not found");
        return;
    }

    console::info("=== FIELD SCAN (hero at 0x%llX) ===", hero);

    for (int i = 0; i < FIELD_COUNT; i++) {
        auto& f = FIELDS[i];
        uintptr_t addr = hero + f.offset;

        if (f.size == 0) {
            // String field - read 16 chars
            char buf[17]{};
            if (game::is_valid_ptr(addr)) {
                memcpy(buf, reinterpret_cast<void*>(addr), 16);
                buf[16] = 0;
                console::info("  [0x%03X] %-20s = \"%s\"  (%s)", f.offset, f.name, buf, f.desc);
            } else {
                console::warn("  [0x%03X] %-20s = <invalid ptr>", f.offset, f.name);
            }
        } else if (f.size == 1) {
            uint8_t val = 0;
            game::safe_read<uint8_t>(addr, val);
            console::info("  [0x%03X] %-20s = %u  (%s)", f.offset, f.name, val, f.desc);
        } else if (f.size == 2) {
            uint16_t val = 0;
            game::safe_read<uint16_t>(addr, val);
            console::info("  [0x%03X] %-20s = %u  (%s)", f.offset, f.name, val, f.desc);
        } else if (f.size == 4) {
            int32_t val = 0;
            game::safe_read<int32_t>(addr, val);
            console::info("  [0x%03X] %-20s = %d (0x%X)  (%s)", f.offset, f.name, val, val, f.desc);
        } else if (f.size == 8) {
            int64_t val = 0;
            game::safe_read<int64_t>(addr, val);
            console::info("  [0x%03X] %-20s = %lld (0x%llX)  (%s)", f.offset, f.name, val, val, f.desc);
        }
    }

    // === Dump raw bytes around name area to find the character name ===
    console::info("");
    console::info("=== NAME SEARCH (raw dump 0x68-0xD8) ===");
    for (int off = 0x68; off < 0xD8; off += 16) {
        char hex_buf[128]{};
        char ascii_buf[20]{};
        int pos = 0;
        for (int j = 0; j < 16; j++) {
            uint8_t b = 0;
            game::safe_read<uint8_t>(hero + off + j, b);
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", b);
            ascii_buf[j] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        ascii_buf[16] = 0;
        console::info("  [0x%03X] %s |%s|", off, hex_buf, ascii_buf);
    }

    // Also try reading std::string at offsets that showed <bad_ptr>
    // The issue might be that the string uses a different SSO threshold
    // Let's dump the raw 32 bytes at each candidate to diagnose
    console::info("");
    console::info("=== STRING CANDIDATES (raw 32-byte dump) ===");
    int str_offsets[] = {0x68, 0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8};
    for (int off : str_offsets) {
        uint64_t qw0 = 0, qw1 = 0, qw2 = 0, qw3 = 0;
        game::safe_read<uint64_t>(hero + off, qw0);
        game::safe_read<uint64_t>(hero + off + 8, qw1);
        game::safe_read<uint64_t>(hero + off + 16, qw2);
        game::safe_read<uint64_t>(hero + off + 24, qw3);

        // Try to read as SSO string (first 16 bytes are the data if capacity <= 15)
        char sso[17]{};
        memcpy(sso, reinterpret_cast<void*>(hero + off), 16);
        sso[16] = 0;
        // Filter printable
        bool has_text = false;
        for (int j = 0; j < 16; j++) {
            if (sso[j] >= 'A' && sso[j] <= 'z') { has_text = true; break; }
        }

        // Also try dereferencing qw0 as pointer to string
        char heap_str[32]{};
        bool heap_ok = false;
        if (qw0 > 0x10000 && game::is_valid_ptr(qw0)) {
            memcpy(heap_str, reinterpret_cast<void*>(qw0), 20);
            heap_str[20] = 0;
            bool hp = false;
            for (int j = 0; j < 20; j++) {
                if (heap_str[j] >= 'A' && heap_str[j] <= 'z') { hp = true; break; }
            }
            heap_ok = hp;
        }

        if (has_text || heap_ok) {
            console::info("  [0x%03X] q0=0x%llX q1=0x%llX q2=%lld q3=%lld", off, qw0, qw1, qw2, qw3);
            if (has_text)
                console::info("          SSO: \"%s\"", sso);
            if (heap_ok)
                console::info("          HEAP: \"%s\"", heap_str);
        }
    }

    // === Search for silver (scan ENTIRE hero object for value 5000 as int32 and int64) ===
    console::info("");
    console::info("=== VALUE SEARCH (entire hero, 0x0 - 0x37B8) ===");
    // Search as int32
    for (int off = 0; off < 0x37B8; off += 4) {
        int32_t val = 0;
        if (game::safe_read<int32_t>(hero + off, val) && val == 5000) {
            console::ok("  [0x%04X] int32 = %d  ** MATCH **", off, val);
        }
    }
    // Search as int64
    for (int off = 0; off < 0x37B8; off += 8) {
        int64_t val = 0;
        if (game::safe_read<int64_t>(hero + off, val) && val == 5000) {
            console::ok("  [0x%04X] int64 = %lld  ** MATCH **", off, val);
        }
    }

    console::info("=== END SCAN ===");
    console::info("Verify values against what you see in-game.");
    console::info("If a value is wrong, use Search Int/String to find the correct offset.");
}

// Scan a range of offsets looking for a specific value
inline void search_int(int target, int start_offset = 0, int end_offset = 14240, int step = 4) {
    uintptr_t hero = game::get_hero();
    if (!hero) return;

    console::info("[SEARCH] Looking for int value %d (0x%X) in hero+0x%X to hero+0x%X",
                  target, target, start_offset, end_offset);
    int found = 0;
    for (int off = start_offset; off < end_offset; off += step) {
        int32_t val = 0;
        if (game::safe_read<int32_t>(hero + off, val) && val == target) {
            console::ok("  FOUND at hero+0x%X (%d)", off, off);
            found++;
            if (found >= 10) {
                console::info("  ... (stopping at 10 results)");
                break;
            }
        }
    }
    if (!found)
        console::warn("  Not found");
}

// Scan for a string in the hero struct
inline void search_string(const char* target, int start_offset = 0, int end_offset = 14240) {
    uintptr_t hero = game::get_hero();
    if (!hero) return;

    int target_len = static_cast<int>(strlen(target));
    console::info("[SEARCH] Looking for string \"%s\" in hero+0x%X to hero+0x%X",
                  target, start_offset, end_offset);
    int found = 0;
    for (int off = start_offset; off < end_offset; off++) {
        if (!game::is_valid_ptr(hero + off)) continue;
        char* ptr = reinterpret_cast<char*>(hero + off);
        if (memcmp(ptr, target, target_len) == 0) {
            console::ok("  FOUND at hero+0x%X (%d)", off, off);
            found++;
            if (found >= 5) break;
        }
    }
    if (!found)
        console::warn("  Not found");
}

} // namespace scanner
