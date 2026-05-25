#pragma once
#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <string>
#include <cstring>
#include <vector>
#include <set>
#include <fstream>
#include <nlohmann/json.hpp>

// Forward declarations - defined in dllmain.cpp, called from hooked_process
void pathfinder_tick_from_process(uintptr_t hero);
void bot_tick_from_process(uintptr_t hero);

namespace game {

// ═══════════════════════════════════════════════════════════════════════
// OFFSETS - All confirmed via runtime testing or IDA analysis
// ═══════════════════════════════════════════════════════════════════════

// Hero singleton
constexpr uintptr_t OFF_HERO_PTR       = 0x4DF588;   // global shared_ptr (first qword = raw ptr)
constexpr int        VTABLE_PROCESS    = 1;           // CHero::Process vtable index

// Function addresses (base-relative)
constexpr uintptr_t FN_JUMP            = 0x22A0B0;   // void(this, int x, int y)
constexpr uintptr_t FN_WALK_RUN        = 0x229DF0;   // void(this, int x, int y)
constexpr uintptr_t FN_SETUP_BOOTH     = 0x1866E0;   // NOT magic attack - sets up market booth
constexpr uintptr_t FN_USE_ITEM        = 0x188F30;   // void(this, uint itemId)
constexpr uintptr_t FN_ITEM_LOOKUP     = 0x187E10;   // ptr(this, outPtr, itemId, flag)
constexpr uintptr_t FN_INTERACT_SEND   = 0x1C2D50;   // Themida VM - sends MsgInteract
constexpr uintptr_t FN_MSG_INTERACT_CTOR = 0x1C7240; // MsgInteract constructor
constexpr uintptr_t FN_MSG_INTERACT_CREATE = 0x1C76B0; // MsgInteract.Create(msg, type, heroId, targetId, posXY)
constexpr uintptr_t FN_SEND_QUEUE     = 0x18EEA0;    // queue packet for network send
constexpr uintptr_t FN_GET_SEND_MGR   = 0x0B9490;    // get network send manager singleton
constexpr uintptr_t FN_ITEM_ACTION     = 0x1CBE50;   // Themida VM - sends MsgItem
constexpr uintptr_t FN_CAN_PK          = 0x1C2520;   // Themida VM - int(ctx, heroId, x, y)
constexpr uintptr_t FN_VIP_TELEPORT    = 0x17FE60;   // void(hero, int cityIdx) — VIP teleport
constexpr uintptr_t FN_ACTIVATE_NPC    = 0x1B6810;   // void(hero, uint npcId) — activate/open NPC (sends MsgNpc mode=1)
constexpr uintptr_t FN_ANSWER_NPC      = 0x1B66B0;   // void(hero, optionId, textPtr) — answer task dialog
constexpr uintptr_t FN_WH_QUERY       = 0x1BDA50;   // void(hero, uint npcId) — warehouse query
constexpr uintptr_t FN_WH_STORE       = 0x1BDF00;   // void(hero, uint itemId) — deposit item to warehouse
constexpr uintptr_t FN_WH_RETRIEVE    = 0x1BE0E0;   // void(hero, uint itemId) — withdraw item from warehouse
constexpr uintptr_t FN_WH_CLEAR       = 0x1BDD40;   // char(hero) — clear/close warehouse
constexpr uintptr_t FN_NETWORK_SEND    = 0x1DC160;   // void(char* buf) - raw send
// (duplicates removed)

// Field offsets (from hero pointer)
constexpr uintptr_t OFF_STATUS_FLAG    = 0x30;        // int64 - status bitmask
constexpr uintptr_t OFF_ID             = 0x68;        // uint32 - character ID
constexpr uintptr_t OFF_TRANSFORM      = 0x6C;        // uint32 - transform look
constexpr uintptr_t OFF_NAME           = 0x94;        // char[16] - character name
constexpr uintptr_t OFF_POS_X          = 0xD8;        // int - cell X (posGame)
constexpr uintptr_t OFF_POS_Y          = 0xDC;        // int - cell Y (posGame)
constexpr uintptr_t OFF_WORLD_X        = 0xE0;        // int - world pixel X
constexpr uintptr_t OFF_WORLD_Y        = 0xE4;        // int - world pixel Y
constexpr uintptr_t OFF_SCR_X          = 0xE8;        // int - screen X
constexpr uintptr_t OFF_SCR_Y          = 0xEC;        // int - screen Y

// CCommand (cmdProc) - found from Walk/Run function analysis
constexpr uintptr_t OFF_CMD_TYPE       = 0x188;       // int - cmdProc.iType
constexpr uintptr_t OFF_CMD_STATUS     = 0x18C;       // int - cmdProc.iStatus
constexpr uintptr_t OFF_CMD_TARGET     = 0x190;       // uint32 - cmdProc.idTarget
constexpr uintptr_t OFF_CMD_TARGET_X   = 0x194;       // int - cmdProc.posTarget.x
constexpr uintptr_t OFF_CMD_TARGET_Y   = 0x198;       // int - cmdProc.posTarget.y

constexpr uintptr_t OFF_LOOK_TYPE      = 0x74;        // uint16 - NPC/monster appearance ID
constexpr uintptr_t OFF_TYPE           = 0x2A8;       // int - role type: 6=Hero, 7=Player
constexpr uintptr_t OFF_ROLE_KIND      = 0x2AC;       // int - sub-type/role kind
constexpr uintptr_t OFF_NPC_SORT       = 0x2B0;       // int - NPC sort: 0=None,1=Shop,2=Task,3=Warehouse,6=Forge

// Minimap NPC list (circular deque on hero object)
// From IDA: sub_1400564C0 returns hero ptr, reads [rax+0xE30/E38/E40/E48]
constexpr uintptr_t OFF_MINIMAP_NPC_BUF   = 0xE30;
constexpr uintptr_t OFF_MINIMAP_NPC_MASK  = 0xE38;
constexpr uintptr_t OFF_MINIMAP_NPC_HEAD  = 0xE40;
constexpr uintptr_t OFF_MINIMAP_NPC_COUNT = 0xE48;

// GameMap offsets (GameMap singleton at base+0x4E02E0)
constexpr uintptr_t OFF_GAMEMAP_BASE   = 0x4E02E0;
constexpr uintptr_t OFF_MAP_ID         = 0x200;       // uint32 - map unique ID (e.g. 1002=TwinCity)
constexpr uintptr_t OFF_MAP_DOC_ID     = 0x204;       // uint32 - map document ID (for .dmap file)

// Role kind values (at OFF_ROLE_KIND / +0x2AC)
constexpr int ROLE_KIND_NONE     = 0;
constexpr int ROLE_KIND_GENERIC  = 1;
constexpr int ROLE_KIND_MONSTER  = 4;
constexpr int ROLE_KIND_PET      = 5;
constexpr int ROLE_KIND_GUARD    = 6;
constexpr int ROLE_KIND_BOOTH    = 8;
constexpr int ROLE_KIND_NPC      = 9;
constexpr uintptr_t OFF_LEVEL_PROF     = 0x6D4;       // int - class*1000 + level
constexpr uintptr_t OFF_STAMINA        = 0x6E0;       // int - current stamina
constexpr uintptr_t OFF_MAX_STAMINA    = 0x6E4;       // int - max stamina
constexpr uintptr_t OFF_SILVER         = 0xA30;       // int64 - silver/money
constexpr uintptr_t OFF_INV_COUNT      = 0xB70;       // uint64 - inventory hash map entry count
constexpr uintptr_t OFF_PK_MODE        = 0xD70;       // int - PK mode
constexpr uintptr_t OFF_NPC_ACTIVE     = 0x1000;      // int - 1 when NPC dialog open
constexpr uintptr_t OFF_NPC_ID         = 0x3724;      // uint32 - active NPC ID
constexpr uintptr_t OFF_VIP            = 0x3740;      // int - VIP status
constexpr uintptr_t OFF_WH_TYPE       = 0xFD8;       // uint8 - warehouse type (10=Storage)
constexpr uintptr_t OFF_WH_PKG_ID     = 0xFDC;       // uint32 - warehouse NPC/package ID (0=closed)

// Equipment shared_ptr<CItem> array: hero + 0xB88 + 16*(slot-1), slots 1-8
// 1=Helmet 2=Necklace 3=Armor 4=RightHand 5=LeftHand 6=Ring 7=Boots 8=Garment
constexpr uintptr_t OFF_EQUIP_ITEM_BASE = 0xB88;      // shared_ptr<CItem> array, 16 bytes per slot
constexpr int       EQUIP_RIGHT_HAND    = 4;
constexpr int       EQUIP_LEFT_HAND     = 5;

// HP: current HP is in a stats sub-object at *(hero+0x968), field +8
// MaxHP: hero+0x3D0 (976) when condition met, else calculated from stats
// MP: hero+0xCA8 (3240)
constexpr uintptr_t OFF_STATS_PTR      = 0x968;       // ptr to stats sub-object
constexpr uintptr_t OFF_MAX_HP         = 0x3D0;       // int - max HP (direct)
constexpr uintptr_t OFF_MP             = 0xCA8;       // int - current mana

// GetHP function and GetMaxHP function (call these instead of raw reads)
constexpr uintptr_t FN_GET_HP          = 0x1F1490;    // int(stats_ptr, 1)
constexpr uintptr_t FN_GET_MAX_HP      = 0x179980;    // int(hero_ptr)

// Command types
constexpr int CMD_PICKUP    = 1;
constexpr int CMD_STANDBY   = 2;
constexpr int CMD_WALK      = 3;
constexpr int CMD_RUN       = 4;
constexpr int CMD_JUMP      = 16;
constexpr int CMD_ATTACK    = 20;
constexpr int CMD_SHOOT     = 21;
constexpr int CMD_LOCKATK   = 24;

// Command status
constexpr int CMDSTATUS_BEGIN      = 0;
constexpr int CMDSTATUS_ACCOMPLISH = 6;

// Status flags
constexpr int64_t STATUS_CRIME     = 1LL << 0;
constexpr int64_t STATUS_POISON    = 1LL << 1;
constexpr int64_t STATUS_INVISIBLE = 1LL << 2;
constexpr int64_t STATUS_DIE       = 1LL << 3;
constexpr int64_t STATUS_XPFULL    = 1LL << 4;
constexpr int64_t STATUS_DEAD      = 1LL << 5;
constexpr int64_t STATUS_GHOST     = 1LL << 10;
constexpr int64_t STATUS_RED       = 1LL << 14;
constexpr int64_t STATUS_BLACK     = 1LL << 15;
constexpr int64_t STATUS_CYCLONE   = 1LL << 23;
constexpr int64_t STATUS_FLY       = 1LL << 27;
constexpr int64_t STATUS_FROZEN    = 1LL << 29;

// ═══════════════════════════════════════════════════════════════════════
// MEMORY ACCESS
// ═══════════════════════════════════════════════════════════════════════

struct CMyPos { int x; int y; };

inline uintptr_t get_base() {
    static uintptr_t base = 0;
    if (!base) {
        uintptr_t peb = static_cast<uintptr_t>(__readgsqword(0x60));
        base = *reinterpret_cast<uintptr_t*>(peb + 0x10);
    }
    return base;
}

inline bool is_valid_ptr(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

template<typename T>
inline bool safe_read(uintptr_t addr, T& out) {
    if (!is_valid_ptr(addr)) return false;
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<typename T>
inline bool safe_write(uintptr_t addr, const T& val) {
    if (!is_valid_ptr(addr)) return false;
    __try {
        *reinterpret_cast<T*>(addr) = val;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline uintptr_t get_hero() {
    uintptr_t ptr_addr = get_base() + OFF_HERO_PTR;
    uintptr_t hero = 0;
    if (!safe_read(ptr_addr, hero)) return 0;
    if (!hero || !is_valid_ptr(hero)) return 0;
    return hero;
}

// ═══════════════════════════════════════════════════════════════════════
// FIELD ACCESSORS
// ═══════════════════════════════════════════════════════════════════════

inline bool get_pos(int& x, int& y) {
    uintptr_t hero = get_hero();
    if (!hero) return false;
    if (!safe_read<int>(hero + OFF_POS_X, x)) return false;
    if (!safe_read<int>(hero + OFF_POS_Y, y)) return false;
    return (x > 0 && x < 20000 && y > 0 && y < 20000);
}

inline uint32_t get_id() {
    uintptr_t h = get_hero(); uint32_t v = 0;
    if (h) safe_read(h + OFF_ID, v);
    return v;
}

inline std::string get_name() {
    uintptr_t h = get_hero();
    if (!h || !is_valid_ptr(h + OFF_NAME)) return "";
    char buf[17]{}; memcpy(buf, reinterpret_cast<void*>(h + OFF_NAME), 16);
    return std::string(buf);
}

inline int64_t get_status_flag() {
    uintptr_t h = get_hero(); int64_t v = 0;
    if (h) safe_read(h + OFF_STATUS_FLAG, v);
    return v;
}

inline int64_t get_silver()      { uintptr_t h = get_hero(); int64_t v = 0; if (h) safe_read(h + OFF_SILVER, v); return v; }
inline int get_stamina()         { uintptr_t h = get_hero(); int v = 0; if (h) safe_read(h + OFF_STAMINA, v); return v; }
inline int get_max_stamina()     { uintptr_t h = get_hero(); int v = 0; if (h) safe_read(h + OFF_MAX_STAMINA, v); return v; }
inline int get_pk_mode()         { uintptr_t h = get_hero(); int v = 0; if (h) safe_read(h + OFF_PK_MODE, v); return v; }
inline bool is_npc_active()      { uintptr_t h = get_hero(); int v = 0; if (h) safe_read(h + OFF_NPC_ACTIVE, v); return v != 0; }
inline uint32_t get_active_npc() { uintptr_t h = get_hero(); uint32_t v = 0; if (h) safe_read(h + OFF_NPC_ID, v); return v; }
inline bool is_vip()             { uintptr_t h = get_hero(); int v = 0; if (h) safe_read(h + OFF_VIP, v); return v != 0; }
inline bool is_warehouse_open()  { uintptr_t h = get_hero(); uint32_t v = 0; if (h) safe_read(h + OFF_WH_PKG_ID, v); return v != 0; }
inline uint32_t get_warehouse_id() { uintptr_t h = get_hero(); uint32_t v = 0; if (h) safe_read(h + OFF_WH_PKG_ID, v); return v; }

struct MinimapNpc {
    uint32_t id{0};     // NPC ID at entry+0x20
    int x{0}, y{0};     // position (need to verify offsets)
    char name[17]{};
};

inline std::vector<MinimapNpc> get_minimap_npcs() {
    std::vector<MinimapNpc> result;
    uintptr_t h = get_hero();
    if (!h) return result;

    uintptr_t buffer = 0;
    uint64_t mask = 0, head = 0, count = 0;
    safe_read(h + OFF_MINIMAP_NPC_BUF, buffer);
    safe_read(h + OFF_MINIMAP_NPC_MASK, mask);
    safe_read(h + OFF_MINIMAP_NPC_HEAD, head);
    safe_read(h + OFF_MINIMAP_NPC_COUNT, count);

    if (!buffer || !is_valid_ptr(buffer) || count == 0 || count > 500) return result;

    for (uint64_t i = 0; i < count; i++) {
        uintptr_t node_ptr = 0;
        safe_read(buffer + (mask & (head + i)) * 8, node_ptr);
        if (!node_ptr) continue;

        // Node is a shared_ptr: [0]=entity_ptr, [8]=control_block
        uintptr_t entity = 0;
        safe_read(node_ptr, entity);
        if (!entity || !is_valid_ptr(entity)) continue;

        MinimapNpc npc;
        // The entity has ID at +0x20 (from the minimap render code: [rbx+20h])
        safe_read<uint32_t>(entity + 0x20, npc.id);
        if (npc.id == 0) continue;

        // Try reading name and position - dump first to verify
        // For now just read ID and raw data
        result.push_back(npc);
    }
    return result;
}

inline uint32_t get_map_id() {
    uintptr_t gm = get_base() + OFF_GAMEMAP_BASE;
    uint32_t v = 0;
    safe_read(gm + OFF_MAP_ID, v);
    return v;
}
inline uint32_t get_map_doc_id() {
    uintptr_t gm = get_base() + OFF_GAMEMAP_BASE;
    uint32_t v = 0;
    safe_read(gm + OFF_MAP_DOC_ID, v);
    return v;
}

// ── Map Portals (loaded from map/metadata/*.json) ───────────────────
struct MapPortal {
    std::string name;
    int x{0}, y{0};
    uint32_t portal_id{0};
    std::string type;
    // Discovered destination (from Gateways.json)
    uint32_t dest_map_id{0};
    int dest_x{0}, dest_y{0};
};

inline std::vector<MapPortal> g_map_portals;
inline uint32_t g_portals_map_id{0};
inline std::string g_client_path;
inline std::string g_data_path; // GUI directory for Gateways.json etc

// Parse portals directly from the .dmap binary file
// Format: u64 version, char[260] puzzle, u32 w, u32 h, cells[w*h]*6+checksum*h, u32 portalCount, portal[]{u32 x, u32 y, u32 idx}
inline std::vector<MapPortal> parse_dmap_portals(const std::string& dmap_path) {
    std::vector<MapPortal> result;
    std::ifstream f(dmap_path, std::ios::binary);
    if (!f.is_open()) return result;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 276) return result; // min: 8 + 260 + 4 + 4

    size_t off = 0;
    // uint64 version
    off += 8;
    // char[260] puzzle filename (fixed size, null-terminated inside)
    off += 260;
    // uint32 width, height
    if (off + 8 > data.size()) return result;
    uint32_t w = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
    uint32_t h = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
    if (w == 0 || h == 0 || w > 2000 || h > 2000) return result;

    // Skip cells: each row = w * 6 bytes (cell) + 4 bytes (checksum)
    size_t cells_size = (size_t)h * ((size_t)w * 6 + 4);
    off += cells_size;

    if (off + 4 > data.size()) return result;
    uint32_t portal_count = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
    if (portal_count > 256) portal_count = 256;

    for (uint32_t i = 0; i < portal_count; i++) {
        if (off + 12 > data.size()) break;
        uint32_t px = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
        uint32_t py = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
        uint32_t idx = *reinterpret_cast<uint32_t*>(data.data() + off); off += 4;
        MapPortal p;
        p.name = "Portal#" + std::to_string(idx);
        p.x = static_cast<int>(px);
        p.y = static_cast<int>(py);
        p.portal_id = 0; // assigned later by server SyncTrap
        p.type = "dmap_portal";
        result.push_back(std::move(p));
    }
    return result;
}

inline void load_portals_for_map(uint32_t map_id, const std::string& client_path) {
    g_map_portals.clear();
    g_portals_map_id = map_id;

    // Method 1: Read from map metadata JSON (has names)
    std::string path = client_path + "\\map\\metadata\\" + std::to_string(map_id) + ".json";
    std::ifstream f(path);
    if (f.is_open()) {
        try {
            auto j = nlohmann::json::parse(f);
            if (j.contains("metadata")) {
                for (auto& e : j["metadata"]) {
                    std::string type = e.value("type", "");
                    if (type.find("portal") != std::string::npos) {
                        MapPortal p;
                        p.name = e.value("name", "");
                        p.x = e.value("position_x", 0);
                        p.y = e.value("position_y", 0);
                        p.type = type;
                        g_map_portals.push_back(std::move(p));
                    }
                }
            }
        } catch (...) {}
        // Got portals from metadata — skip dmap fallback
    }

    if (g_map_portals.empty()) {
        // Method 2: Parse portals from .dmap file using GameMap.json to find filename
        console::info("[PORTAL] No metadata, trying dmap file...");
        uint32_t doc_id = get_map_doc_id();
        std::string gamemap_json = client_path + "\\ini\\GameMap.json";
        std::ifstream gmf(gamemap_json);
        if (gmf.is_open()) {
            try {
                auto gj = nlohmann::json::parse(gmf);
                for (auto& entry : gj) {
                    if (entry.value("DocumentId", 0u) == doc_id) {
                        std::string dmap_fn = entry.value("FileName", "");
                        if (!dmap_fn.empty()) {
                            for (auto& c : dmap_fn) if (c == '/') c = '\\';
                            std::string dmap_path = client_path + "\\" + dmap_fn;
                            g_map_portals = parse_dmap_portals(dmap_path);
                            console::info("[PORTAL] Parsed %d portals from %s",
                                (int)g_map_portals.size(), dmap_fn.c_str());
                        }
                        break;
                    }
                }
            } catch (...) {
                console::warn("[PORTAL] Failed to parse GameMap.json");
            }
        }
    }

    // Merge discovered destinations from Gateways.json (stored in data dir)
    std::string gw_path = g_data_path.empty() ? (client_path + "\\Gateways.json") : (g_data_path + "\\Gateways.json");
    std::ifstream gwf(gw_path);
    if (gwf.is_open()) {
        try {
            auto arr = nlohmann::json::parse(gwf);
            for (auto& gw : arr) {
                if (gw.value("MapId", 0u) != map_id) continue;
                int gx = gw.value("X", 0), gy = gw.value("Y", 0);
                uint32_t dest = gw.value("DestMapId", 0u);
                int dx = gw.value("DestX", 0), dy = gw.value("DestY", 0);
                if (!dest) continue;
                // Find matching portal and set destination
                bool found = false;
                for (auto& p : g_map_portals) {
                    if (p.x == gx && p.y == gy) {
                        p.dest_map_id = dest;
                        p.dest_x = dx;
                        p.dest_y = dy;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // Portal not in dmap/metadata — add it
                    MapPortal p;
                    p.name = "Discovered";
                    p.x = gx; p.y = gy;
                    p.type = "discovered";
                    p.dest_map_id = dest;
                    p.dest_x = dx;
                    p.dest_y = dy;
                    g_map_portals.push_back(std::move(p));
                }
            }
        } catch (...) {}
    }
}

// Forward declare (defined in PendingAction section below)
inline void queue_jump(int x, int y);

// ── Portal Explorer (auto-discover destinations) ────────────────────
struct PortalExploreState {
    bool active{false};
    int portal_idx{-1};
    uint32_t source_map_id{0};
    int portal_x{0}, portal_y{0};
    std::string portal_name;
    int64_t step_time{0};
    enum Phase { Idle, Walking, Stepping, WaitMapChange } phase{Idle};
    // Discovery result
    bool discovered{false};
    uint32_t dest_map_id{0};
    int dest_x{0}, dest_y{0};
};
inline PortalExploreState g_portal_explore;

inline void start_portal_explore(int idx) {
    if (idx < 0 || idx >= (int)g_map_portals.size()) return;
    auto& p = g_map_portals[idx];
    g_portal_explore.active = true;
    g_portal_explore.portal_idx = idx;
    g_portal_explore.source_map_id = get_map_id();
    g_portal_explore.portal_x = p.x;
    g_portal_explore.portal_y = p.y;
    g_portal_explore.portal_name = p.name;
    g_portal_explore.phase = PortalExploreState::Walking;
    g_portal_explore.step_time = 0;
    console::info("[EXPLORE] Starting portal exploration: %s (%d,%d) on map %u",
        p.name.c_str(), p.x, p.y, g_portal_explore.source_map_id);
}

inline void stop_portal_explore() {
    g_portal_explore.active = false;
    g_portal_explore.phase = PortalExploreState::Idle;
}

// ── Explore All state machine ────────────────────────────────────────
struct ExploreAllState {
    bool active{false};
    int maps_explored{0};
    int portals_discovered{0};
};
inline ExploreAllState g_explore_all;

inline void start_explore_all() {
    g_explore_all.active = true;
    g_explore_all.maps_explored = 0;
    g_explore_all.portals_discovered = 0;
    console::ok("[EXPLORE-ALL] Started — will explore all unknown portals recursively");
}

inline void stop_explore_all() {
    g_explore_all.active = false;
    stop_portal_explore();
    console::ok("[EXPLORE-ALL] Stopped (%d maps, %d portals discovered)",
        g_explore_all.maps_explored, g_explore_all.portals_discovered);
}

// Called when a single portal exploration completes (from dllmain discovery handler)
inline void explore_all_on_discovered() {
    if (!g_explore_all.active) return;
    g_explore_all.portals_discovered++;
    // The next tick will load portals on the new map and continue
}

// Called each tick from dllmain — drives the explore-all loop
inline void explore_all_tick() {
    if (!g_explore_all.active) return;
    // Don't interfere while a single portal explore is in progress
    if (g_portal_explore.active) return;
    // Don't interfere while pathfinding (checked via g_portal_explore.phase == Walking)

    // Load portals for current map if not loaded
    uint32_t cur_map = get_map_id();
    if (g_portals_map_id != cur_map && !g_client_path.empty()) {
        load_portals_for_map(cur_map, g_client_path);
        g_explore_all.maps_explored++;
        console::info("[EXPLORE-ALL] Loaded %d portals for map %u", (int)g_map_portals.size(), cur_map);
    }

    // Find first portal with unknown destination
    int unknown_idx = -1;
    for (int i = 0; i < (int)g_map_portals.size(); i++) {
        if (g_map_portals[i].dest_map_id == 0) {
            unknown_idx = i;
            break;
        }
    }

    if (unknown_idx >= 0) {
        auto& p = g_map_portals[unknown_idx];
        console::info("[EXPLORE-ALL] Next: %s (%d,%d) on map %u",
            p.name.c_str(), p.x, p.y, cur_map);
        start_portal_explore(unknown_idx);
        // Pathfinding will be started by dllmain when it sees active explore with Walking phase
    } else {
        // All portals on this map are known — done!
        console::ok("[EXPLORE-ALL] All portals on map %u are known!", cur_map);
        stop_explore_all();
    }
}

// Called from process hook each frame
inline void portal_explore_tick(uintptr_t hero) {
    if (!g_portal_explore.active) return;

    int hx = 0, hy = 0;
    safe_read<int>(hero + OFF_POS_X, hx);
    safe_read<int>(hero + OFF_POS_Y, hy);
    int64_t now = static_cast<int64_t>(GetTickCount64());

    switch (g_portal_explore.phase) {
    case PortalExploreState::Walking: {
        int gd = std::max(std::abs(hx - g_portal_explore.portal_x), std::abs(hy - g_portal_explore.portal_y));
        if (gd <= 1) {
            // Arrived at portal — step onto it
            g_portal_explore.phase = PortalExploreState::Stepping;
            g_portal_explore.step_time = now;
            console::info("[EXPLORE] At portal, stepping onto it...");
            // Jump directly onto portal position
            queue_jump(g_portal_explore.portal_x, g_portal_explore.portal_y);
        }
        // Pathfinder handles the walking (started via pathfinder::start_pathfind)
        break;
    }
    case PortalExploreState::Stepping: {
        // We jumped onto the portal, wait for map change
        uint32_t cur_map = get_map_id();
        if (cur_map != g_portal_explore.source_map_id && cur_map != 0) {
            // Map changed — we discovered the destination!
            console::ok("[EXPLORE] Portal '%s' leads to map %u at (%d,%d)!",
                g_portal_explore.portal_name.c_str(), cur_map, hx, hy);

            // Store discovery result for dllmain to process
            g_portal_explore.discovered = true;
            g_portal_explore.dest_map_id = cur_map;
            g_portal_explore.dest_x = hx;
            g_portal_explore.dest_y = hy;
            g_portal_explore.active = false;
            g_portal_explore.phase = PortalExploreState::Idle;
            return;
        }
        // Timeout after 5 seconds
        if (now - g_portal_explore.step_time > 5000) {
            console::warn("[EXPLORE] Portal didn't trigger map change, retrying...");
            queue_jump(g_portal_explore.portal_x, g_portal_explore.portal_y);
            g_portal_explore.step_time = now;
        }
        break;
    }
    default: break;
    }
}

inline uint32_t get_equip_type(int slot) {
    uintptr_t h = get_hero();
    if (!h || slot < 1 || slot > 8) return 0;
    uintptr_t item_ptr = 0;
    safe_read(h + OFF_EQUIP_ITEM_BASE + 16 * (slot - 1), item_ptr);
    if (!item_ptr) return 0;
    uint32_t type_id = 0;
    safe_read(item_ptr + 0x10, type_id);  // CItem+0x10 = typeId
    return type_id;
}
inline uint32_t get_weapon_type() { return get_equip_type(EQUIP_RIGHT_HAND); }

// HP/MP accessors - HP uses a function call, so must be called carefully
// For the Process hook (game thread), these are safe to call
inline int get_stat(int stat_id) {
    uintptr_t h = get_hero();
    if (!h) return 0;
    uintptr_t stats = 0;
    safe_read(h + OFF_STATS_PTR, stats);
    if (!stats || !is_valid_ptr(stats)) return 0;

    using GetStat_t = int(__fastcall*)(uintptr_t, int);
    auto fn = reinterpret_cast<GetStat_t>(get_base() + FN_GET_HP);
    __try { return fn(stats, stat_id); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline int get_hp()     { return get_stat(1); }  // Life
inline int get_mp()     { return get_stat(3); }  // Mana
inline int get_max_mp() { return get_stat(4); }  // MaxMana

inline int get_max_hp() {
    uintptr_t h = get_hero();
    if (!h) return 0;
    // Try direct read first (hero+0x3D0)
    int val = 0;
    safe_read<int>(h + OFF_MAX_HP, val);
    if (val > 0 && val < 10000000) return val;

    // Fallback: call the game function
    using GetMaxHP_t = int(__fastcall*)(uintptr_t);
    auto fn = reinterpret_cast<GetMaxHP_t>(get_base() + FN_GET_MAX_HP);
    __try { return fn(h); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline int get_level() {
    uintptr_t h = get_hero(); int v = 0;
    if (h) safe_read(h + OFF_LEVEL_PROF, v);
    return v % 1000; // class*1000+level encoding
}
inline int get_profession() {
    uintptr_t h = get_hero(); int v = 0;
    if (h) safe_read(h + OFF_LEVEL_PROF, v);
    return v / 1000;
}
inline uint64_t get_inv_count() {
    uintptr_t h = get_hero(); uint64_t v = 0;
    if (h) safe_read(h + OFF_INV_COUNT, v);
    return v;
}

// ── Magic/Skill list ────────────────────────────────────────────────
// Hero magic vector at hero+0x1918 (std::vector<shared_ptr<CMagic>>)
// Each shared_ptr = 16 bytes [ptr, control_block]
// CMagic layout: +0x00=vtable, +0x08=m_bEnable(BOOL), +0x0C=m_dwExp(DWORD)
// +0x10=MagicTypeInfo { +0x00=idMagicType, +0x04=dwActionSort, +0x08=strName(32bytes),
//   +0x28=dwCrime, +0x2C=dwGround, +0x30=dwMulti, +0x34=dwTarget, +0x38=dwLevel,
//   +0x3C=dwMpCost, +0x40=dwPower, ... +0x50=dwRange, +0x54=dwDistance }
constexpr uintptr_t OFF_MAGIC_VEC = 0x1918; // hero + this = vector begin ptr

struct MagicInfo {
    uint32_t id{0};
    uint32_t level{0};
    uint32_t action_sort{0};
    uint32_t mp_cost{0};
    uint32_t power{0};
    uint32_t range{0};
    uint32_t distance{0};
    uint32_t target_type{0}; // dwTarget
    bool enabled{false};
    char name[32]{};
};

inline std::vector<MagicInfo> get_magic_list() {
    std::vector<MagicInfo> result;
    uintptr_t h = get_hero();
    if (!h) return result;

    uintptr_t vec_begin = 0, vec_end = 0;
    safe_read(h + OFF_MAGIC_VEC, vec_begin);
    safe_read(h + OFF_MAGIC_VEC + 8, vec_end);

    if (!vec_begin || !is_valid_ptr(vec_begin) || vec_end <= vec_begin) return result;
    size_t count = (vec_end - vec_begin) / 16; // shared_ptr = 16 bytes
    if (count > 200) return result;

    for (size_t i = 0; i < count; i++) {
        uintptr_t magic_ptr = 0;
        safe_read(vec_begin + i * 16, magic_ptr);
        if (!magic_ptr || !is_valid_ptr(magic_ptr)) continue;

        MagicInfo mi;
        // CMagic fields
        int32_t enabled = 0;
        safe_read<int32_t>(magic_ptr + 0x08, enabled);
        mi.enabled = (enabled != 0);
        // MagicTypeInfo at CMagic+0x10
        uintptr_t info = magic_ptr + 0x10;
        safe_read<uint32_t>(info + 0x00, mi.id);           // idMagicType
        safe_read<uint32_t>(info + 0x04, mi.action_sort);  // dwActionSort
        // strName at info+0x08 (SSO std::string, first 16 chars in buffer)
        if (is_valid_ptr(info + 0x08))
            memcpy(mi.name, reinterpret_cast<void*>(info + 0x08), 16);
        mi.name[16] = 0;
        safe_read<uint32_t>(info + 0x38, mi.level);        // dwLevel
        safe_read<uint32_t>(info + 0x3C, mi.mp_cost);      // dwMpCost
        safe_read<uint32_t>(info + 0x40, mi.power);        // dwPower
        safe_read<uint32_t>(info + 0x34, mi.target_type);  // dwTarget
        safe_read<uint32_t>(info + 0x50, mi.range);        // dwRange
        safe_read<uint32_t>(info + 0x54, mi.distance);     // dwDistance

        if (mi.id > 0) result.push_back(mi);
    }
    return result;
}

// Find a specific magic by ID, returns nullptr-like empty MagicInfo if not found
inline MagicInfo find_magic(uint32_t magic_id) {
    auto list = get_magic_list();
    for (auto& m : list) {
        if (m.id == magic_id) return m;
    }
    return MagicInfo{};
}

// ── Inventory iteration ─────────────────────────────────────────────
// Hash map at hero+0xB50. Each node: [hash(8), next(8), itemId(4), ...data]
struct InvItem {
    uint32_t item_id{0};    // instance ID (unique)
    uint32_t type_id{0};    // type from itemtype.json
    char name[17]{};        // item name (SSO string from CItem+0x18)
    int amount{0};          // stack count
    int amount_limit{0};    // max stack
};

inline std::vector<InvItem> get_inventory() {
    std::vector<InvItem> result;
    uintptr_t h = get_hero();
    if (!h) return result;

    // Doubly-linked list with sentinel at hero+0xB50
    // Node layout: [+0]=prev, [+8]=next, [+16]=itemId(uint32?), [+24]=shared_ptr<CItem>, [+32]=control_block
    // Sentinel (head) has no item data, real items start at head->next
    uintptr_t sentinel = 0;
    uint64_t count = 0;
    safe_read(h + 0xB50, sentinel);
    safe_read(h + 0xB58, count);

    if (!sentinel || !is_valid_ptr(sentinel) || count == 0 || count > 500) return result;

    // Start from sentinel->next (first real item)
    uintptr_t node = 0;
    safe_read(sentinel + 8, node); // next pointer

    for (uint64_t i = 0; i < count && node && node != sentinel && is_valid_ptr(node); i++) {
        InvItem item;

        // node+16 = item instance ID
        safe_read<uint32_t>(node + 16, item.item_id);

        // node+24 = CItem* (shared_ptr stored pointer)
        // CItem layout: [+0]=vtable, [+8]=instanceId, [+0x10]=typeId, [+0x18]=name(SSO), [+0x28]=amount, [+0x30]=amountLimit
        uintptr_t citem = 0;
        safe_read(node + 24, citem);
        if (citem && is_valid_ptr(citem)) {
            safe_read<uint32_t>(citem + 0x10, item.type_id);
            safe_read<int>(citem + 0x28, item.amount);
            safe_read<int>(citem + 0x30, item.amount_limit);
            if (is_valid_ptr(citem + 0x18))
                memcpy(item.name, reinterpret_cast<void*>(citem + 0x18), 16);
            item.name[16] = 0;
        }

        if (item.item_id > 0)
            result.push_back(item);

        // Follow next pointer at node+8
        uintptr_t next = 0;
        safe_read(node + 8, next);
        node = next;
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// STATE QUERIES
// ═══════════════════════════════════════════════════════════════════════

inline bool is_dead()      { return (get_status_flag() & STATUS_DEAD) != 0; }
inline bool is_ghost()     { return (get_status_flag() & STATUS_GHOST) != 0; }
inline bool is_invisible() { return (get_status_flag() & STATUS_INVISIBLE) != 0; }
inline bool is_flying()    { return (get_status_flag() & STATUS_FLY) != 0; }
inline bool is_cyclone()   { return (get_status_flag() & STATUS_CYCLONE) != 0; }
inline bool is_xp_full()   { return (get_status_flag() & STATUS_XPFULL) != 0; }
inline bool is_frozen()    { return (get_status_flag() & STATUS_FROZEN) != 0; }
inline bool is_red()       { return (get_status_flag() & STATUS_RED) != 0; }
inline bool is_black()     { return (get_status_flag() & STATUS_BLACK) != 0; }
inline bool is_poisoned()  { return (get_status_flag() & STATUS_POISON) != 0; }

inline int get_cmd_type() {
    uintptr_t h = get_hero(); int v = 0;
    if (h) safe_read(h + OFF_CMD_TYPE, v);
    return v;
}

inline int get_cmd_status() {
    uintptr_t h = get_hero(); int v = 0;
    if (h) safe_read(h + OFF_CMD_STATUS, v);
    return v;
}

inline bool is_acting()  { return get_cmd_status() != CMDSTATUS_ACCOMPLISH; }
inline bool is_jumping() { return get_cmd_type() == CMD_JUMP; }
inline bool is_walking() { return get_cmd_type() == CMD_WALK; }
inline bool is_running() { return get_cmd_type() == CMD_RUN; }
inline bool is_moving()  { int t = get_cmd_type(); return t == CMD_WALK || t == CMD_RUN || t == CMD_JUMP; }

inline bool is_attacking() {
    int t = get_cmd_type();
    return t == CMD_ATTACK || t == CMD_LOCKATK || t == CMD_SHOOT;
}

inline CMyPos get_pos_final() {
    CMyPos pos{};
    if (is_jumping()) {
        uintptr_t h = get_hero();
        if (h) {
            safe_read<int>(h + OFF_CMD_TARGET_X, pos.x);
            safe_read<int>(h + OFF_CMD_TARGET_Y, pos.y);
        }
    } else {
        get_pos(pos.x, pos.y);
    }
    return pos;
}

// ═══════════════════════════════════════════════════════════════════════
// TASK DIALOG (NPC dialog reading and responding)
// ═══════════════════════════════════════════════════════════════════════

// Dialog option deque at hero+0x1010..0x1028
constexpr uintptr_t OFF_DIALOG_BUF    = 0x1010;  // deque buffer (ptr array)
constexpr uintptr_t OFF_DIALOG_MASK   = 0x1018;  // deque mask
constexpr uintptr_t OFF_DIALOG_HEAD   = 0x1020;  // deque head index
constexpr uintptr_t OFF_DIALOG_COUNT  = 0x1028;  // deque count

// Dialog response function: void(hero, optionId, textPtr)
constexpr uintptr_t FN_DIALOG_ANSWER  = 0x1B66B0;

struct DialogOption {
    int type{0};        // 1=Button, 2=Edit
    int id{0};          // option ID to send back
    std::string text;   // display text
};

inline bool is_dialog_open() {
    uintptr_t h = get_hero();
    if (!h) return false;
    int active = 0;
    safe_read<int>(h + OFF_NPC_ACTIVE, active);
    return active != 0;
}

inline std::vector<DialogOption> get_dialog_options() {
    std::vector<DialogOption> result;
    uintptr_t h = get_hero();
    if (!h) return result;

    uintptr_t buf = 0, mask = 0, head = 0, count = 0;
    safe_read(h + OFF_DIALOG_BUF, buf);
    safe_read(h + OFF_DIALOG_MASK, mask);
    safe_read(h + OFF_DIALOG_HEAD, head);
    safe_read(h + OFF_DIALOG_COUNT, count);

    if (!buf || !is_valid_ptr(buf) || count == 0 || count > 50) return result;

    // IDA: dec mask; and idx, mask (so mask is capacity, actual mask = mask-1)
    uint64_t actual_mask = mask - 1;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t idx = (head + i) & actual_mask;
        // buf[idx] = pointer to wrapper
        uintptr_t wrapper = 0;
        safe_read(buf + idx * 8, wrapper);
        if (!wrapper || !is_valid_ptr(wrapper)) continue;

        // wrapper[0] = pointer to actual option data
        uintptr_t inner = 0;
        safe_read(wrapper, inner);
        if (!inner || !is_valid_ptr(inner)) continue;

        // Option data: [+0]=type(int), [+8]=id(int), [+0x10]=text(std::string)
        DialogOption opt;
        safe_read<int>(inner, opt.type);
        safe_read<int>(inner + 8, opt.id);

        // Read std::string at inner+0x10 (MSVC SSO layout)
        uint64_t str_cap = 0;
        uint64_t str_len = 0;
        safe_read(inner + 0x28, str_cap);
        safe_read(inner + 0x20, str_len);
        if (str_len > 0 && str_len < 4096) {
            uintptr_t str_data = inner + 0x10;
            if (str_cap >= 16) {
                safe_read(inner + 0x10, str_data);
            }
            if (is_valid_ptr(str_data)) {
                opt.text.resize(str_len);
                memcpy(&opt.text[0], reinterpret_cast<void*>(str_data), str_len);
            }
        }

        if (opt.type == 1 || opt.type == 2)
            result.push_back(std::move(opt));
    }
    return result;
}

inline void answer_dialog(int option_id) {
    uintptr_t h = get_hero();
    if (!h) return;
    uintptr_t base = get_base();
    using AnswerFn = void(__fastcall*)(uintptr_t, int, uintptr_t);
    auto fn = reinterpret_cast<AnswerFn>(base + FN_DIALOG_ANSWER);
    fn(h, option_id, 0);
}

// ═══════════════════════════════════════════════════════════════════════
// PENDING ACTIONS (queued from any thread, executed on game thread)
// ═══════════════════════════════════════════════════════════════════════

enum class ActionType : int {
    None = 0,
    Jump,
    Walk,
    Run,
    Attack,
    PickUp,
    MagicAttack,       // ground target magic (at hero pos)
    MagicAttackTarget,  // targeted magic (on entity)
    UseItem,
    AnswerDialog,
    ActivateNpc,
    AnswerNpc,
    Revive,
    DepositAll,
    VipTeleport,
    WarehouseDeposit,
};

struct PendingAction {
    std::atomic<bool> pending{false};
    ActionType type{ActionType::None};
    int x{0};
    int y{0};
    uint32_t target_id{0};
    uint32_t magic_id{0};
    uint32_t item_id{0};
};

inline PendingAction& get_pending() {
    static PendingAction pa;
    return pa;
}

inline void queue_jump(int x, int y)      { auto& p = get_pending(); p.type = ActionType::Jump; p.x = x; p.y = y; p.pending.store(true); }
inline void queue_walk(int x, int y)      { auto& p = get_pending(); p.type = ActionType::Walk; p.x = x; p.y = y; p.pending.store(true); }
inline void queue_run(int x, int y)       { auto& p = get_pending(); p.type = ActionType::Run;  p.x = x; p.y = y; p.pending.store(true); }
inline void queue_attack(uint32_t id)     { auto& p = get_pending(); p.type = ActionType::Attack; p.target_id = id; p.pending.store(true); }
inline void queue_pickup(uint32_t id, int x, int y) {
    auto& p = get_pending(); p.type = ActionType::PickUp; p.target_id = id; p.x = x; p.y = y; p.pending.store(true);
}
inline void queue_magic_attack(uint32_t magic_id) {
    auto& p = get_pending(); p.type = ActionType::MagicAttack; p.magic_id = magic_id; p.pending.store(true);
}
inline void queue_magic_attack_target(uint32_t magic_id, uint32_t target_id) {
    auto& p = get_pending(); p.type = ActionType::MagicAttackTarget; p.magic_id = magic_id; p.target_id = target_id; p.pending.store(true);
}
inline void queue_use_item(uint32_t item_id) {
    auto& p = get_pending(); p.type = ActionType::UseItem; p.item_id = item_id; p.pending.store(true);
}
inline void queue_revive() {
    auto& p = get_pending(); p.type = ActionType::Revive; p.pending.store(true);
}
inline void queue_answer_dialog(int option_id) {
    auto& p = get_pending(); p.type = ActionType::AnswerDialog; p.target_id = static_cast<uint32_t>(option_id); p.pending.store(true);
}
inline void queue_activate_npc(uint32_t npc_id) {
    auto& p = get_pending(); p.type = ActionType::ActivateNpc; p.target_id = npc_id; p.pending.store(true);
}
inline void queue_answer_npc(int answer) {
    auto& p = get_pending(); p.type = ActionType::AnswerNpc; p.target_id = static_cast<uint32_t>(answer); p.pending.store(true);
}
inline void queue_deposit_all() {
    auto& p = get_pending(); p.type = ActionType::DepositAll; p.pending.store(true);
}

// Warehouse deposit
inline void queue_warehouse_deposit(uint32_t item_id) {
    auto& p = get_pending(); p.type = ActionType::WarehouseDeposit; p.target_id = item_id; p.pending.store(true);
}

// VIP Teleport city indices
constexpr int VIP_CITY_TC = 1;  // Twin City (1002)
constexpr int VIP_CITY_PC = 2;  // Phoenix Castle (1011)
constexpr int VIP_CITY_AC = 3;  // Ape Mountain (1020)
constexpr int VIP_CITY_DC = 4;  // Desert City (1000)
constexpr int VIP_CITY_BI = 5;  // Bird Islands (1015)

inline void queue_vip_teleport(int city_idx) {
    auto& p = get_pending(); p.type = ActionType::VipTeleport; p.x = city_idx; p.pending.store(true);
}

inline int64_t g_last_vip_teleport_time{0};
constexpr int VIP_TELEPORT_COOLDOWN_MS = 60000; // 1 minute

// Death timer tracking
inline int64_t g_death_time = 0; // timestamp when died (GetTickCount64)

inline int get_revive_countdown() {
    if (!is_ghost()) return 0;
    if (g_death_time == 0) return 20;
    int64_t elapsed = static_cast<int64_t>(GetTickCount64()) - g_death_time;
    int remaining = 20 - static_cast<int>(elapsed / 1000);
    return remaining > 0 ? remaining : 0;
}

// ═══════════════════════════════════════════════════════════════════════
// VMT HOOK (CHero::Process) - executes actions on game thread
// ═══════════════════════════════════════════════════════════════════════

using Process_t = void(__fastcall*)(uintptr_t, void*);
using GameFunc_xyi = void(__fastcall*)(uintptr_t, int, int);

using PostProcessCb = void(*)();
inline Process_t g_orig_process = nullptr;
inline uintptr_t* g_vtable_slot = nullptr;
inline PostProcessCb g_post_process_callback = nullptr;
inline std::atomic<uint32_t> g_hook_call_count{0};

inline void __fastcall hooked_process(uintptr_t thisptr, void* pInfo) {
    g_hook_call_count.fetch_add(1, std::memory_order_relaxed);

    if (g_orig_process)
        g_orig_process(thisptr, pInfo);

    // Pathfinder tick - runs every frame on game thread
    ::pathfinder_tick_from_process(thisptr);

    // Bot tick - runs every frame on game thread
    ::bot_tick_from_process(thisptr);

    // Execute pending action on game thread
    auto& pa = get_pending();
    if (!pa.pending.load(std::memory_order_acquire)) return;
    pa.pending.store(false, std::memory_order_release);

    switch (pa.type) {
    case ActionType::Jump: {
        // Use SetCommand via vtable (same as bot's do_jump) — FN_JUMP corrupts gold
        uint8_t jcmd[0x108]{};
        *reinterpret_cast<int32_t*>(jcmd + 0x00) = CMD_JUMP;
        *reinterpret_cast<int32_t*>(jcmd + 0x0C) = pa.x;
        *reinterpret_cast<int32_t*>(jcmd + 0x10) = pa.y;
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(thisptr);
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtable + 0x1D8);
        reinterpret_cast<void(__fastcall*)(uintptr_t, void*)>(fn)(thisptr, jcmd);
        break;
    }
    case ActionType::Walk:
    case ActionType::Run: {
        auto fn = reinterpret_cast<GameFunc_xyi>(get_base() + FN_WALK_RUN);
        fn(thisptr, pa.x, pa.y);
        break;
    }
    case ActionType::Attack: {
        // Write attack command directly into cmdProc
        safe_write<int>(thisptr + OFF_CMD_TYPE, CMD_LOCKATK);
        safe_write<int>(thisptr + OFF_CMD_STATUS, CMDSTATUS_BEGIN);
        safe_write<uint32_t>(thisptr + OFF_CMD_TARGET, pa.target_id);
        break;
    }
    case ActionType::PickUp: {
        safe_write<int>(thisptr + OFF_CMD_TYPE, CMD_PICKUP);
        safe_write<int>(thisptr + OFF_CMD_STATUS, CMDSTATUS_BEGIN);
        safe_write<uint32_t>(thisptr + OFF_CMD_TARGET, pa.target_id);
        safe_write<int>(thisptr + OFF_CMD_TARGET_X, pa.x);
        safe_write<int>(thisptr + OFF_CMD_TARGET_Y, pa.y);
        break;
    }
    case ActionType::MagicAttack: {
        // Ground target magic at hero position - send raw MsgInteract packet
        // Same as MagicAttackTarget but with targetId=0
        uintptr_t b = get_base();
        uint32_t hero_id = 0;
        int hx = 0, hy = 0;
        safe_read<uint32_t>(thisptr + OFF_ID, hero_id);
        safe_read<int>(thisptr + OFF_POS_X, hx);
        safe_read<int>(thisptr + OFF_POS_Y, hy);

        auto write_varint = [](uint8_t* dst, uint32_t val) -> int {
            int n = 0;
            while (val > 0x7F) { dst[n++] = (val & 0x7F) | 0x80; val >>= 7; }
            dst[n++] = val & 0x7F;
            return n;
        };

        uint8_t pb[128];
        int pos = 0;
        pb[pos++] = 0x08; pos += write_varint(pb + pos, hero_id);   // playerId
        pb[pos++] = 0x10; pos += write_varint(pb + pos, 0);          // targetId = 0
        pb[pos++] = 0x18; pos += write_varint(pb + pos, (uint32_t)hx); // x
        pb[pos++] = 0x20; pos += write_varint(pb + pos, (uint32_t)hy); // y
        pb[pos++] = 0x30; pos += write_varint(pb + pos, 21);         // action = MAGIC_ATTACK
        pb[pos++] = 0x38; pos += write_varint(pb + pos, 0);          // damage
        pb[pos++] = 0x40; pos += write_varint(pb + pos, pa.magic_id); // spellId
        pb[pos++] = 0x48; pos += write_varint(pb + pos, 0);          // koCount

        uint16_t total_size = static_cast<uint16_t>(4 + pos);
        uint16_t msg_type = 1022;
        uint8_t packet[256];
        memcpy(packet, &total_size, 2);
        memcpy(packet + 2, &msg_type, 2);
        memcpy(packet + 4, pb, pos);

        using GetMgr_t = uintptr_t(__fastcall*)();
        auto get_mgr = reinterpret_cast<GetMgr_t>(b + FN_GET_SEND_MGR);
        uintptr_t mgr = 0;
        __try { mgr = get_mgr(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (mgr) {
            uintptr_t net_buf = 0;
            safe_read(mgr + 32, net_buf);
            if (net_buf && is_valid_ptr(net_buf)) {
                using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                auto raw_send = reinterpret_cast<RawSend_t>(b + 0x1DBA40);
                __try { raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size)); }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        break;
    }
    case ActionType::MagicAttackTarget: {
        // Construct MsgInteract protobuf manually and send via game's raw send
        // Protobuf fields: playerId(1), targetId(2), x(3), y(4), action(6), damage(7), spellId(8), koCount(9)
        // Packet: [u16 totalSize][u16 type=1022][protobuf body]

        uintptr_t b = get_base();
        uint32_t hero_id = 0;
        int hx = 0, hy = 0;
        safe_read<uint32_t>(thisptr + OFF_ID, hero_id);
        safe_read<int>(thisptr + OFF_POS_X, hx);
        safe_read<int>(thisptr + OFF_POS_Y, hy);

        // Get target position
        int tx = hx, ty = hy;
        uintptr_t ps_addr = b + 0x4DF5F0;
        uintptr_t buf = 0; uint64_t cap = 0, hd = 0, cnt = 0;
        safe_read(ps_addr + 0x10, buf);
        safe_read(ps_addr + 0x18, cap);
        safe_read(ps_addr + 0x20, hd);
        safe_read(ps_addr + 0x28, cnt);
        if (buf && is_valid_ptr(buf) && cnt > 0 && cap > 0) {
            uint64_t m = cap - 1;
            for (uint64_t i = 0; i < cnt; i++) {
                uintptr_t nd = 0;
                safe_read(buf + ((hd + i) & m) * 8, nd);
                if (!nd || !is_valid_ptr(nd)) continue;
                uintptr_t rl = 0; safe_read(nd, rl);
                if (!rl || !is_valid_ptr(rl)) continue;
                uint32_t eid = 0; safe_read<uint32_t>(rl + OFF_ID, eid);
                if (eid == pa.target_id) {
                    safe_read<int>(rl + OFF_POS_X, tx);
                    safe_read<int>(rl + OFF_POS_Y, ty);
                    break;
                }
            }
        }

        // Encode protobuf varint helper
        auto write_varint = [](uint8_t* dst, uint32_t val) -> int {
            int n = 0;
            while (val > 0x7F) { dst[n++] = (val & 0x7F) | 0x80; val >>= 7; }
            dst[n++] = val & 0x7F;
            return n;
        };

        // Build protobuf body
        uint8_t pb[128];
        int pos = 0;
        // field 1: playerId
        pb[pos++] = 0x08; pos += write_varint(pb + pos, hero_id);
        // field 2: targetId
        pb[pos++] = 0x10; pos += write_varint(pb + pos, pa.target_id);
        // field 3: x
        pb[pos++] = 0x18; pos += write_varint(pb + pos, static_cast<uint32_t>(tx));
        // field 4: y
        pb[pos++] = 0x20; pos += write_varint(pb + pos, static_cast<uint32_t>(ty));
        // field 6: action = 21 (Magic)
        pb[pos++] = 0x30; pos += write_varint(pb + pos, 21);
        // field 7: damage = 0
        pb[pos++] = 0x38; pos += write_varint(pb + pos, 0);
        // field 8: spellId
        pb[pos++] = 0x40; pos += write_varint(pb + pos, pa.magic_id);
        // field 9: koCount = 0
        pb[pos++] = 0x48; pos += write_varint(pb + pos, 0);

        // Build packet: [u16 size][u16 type][protobuf body]
        uint16_t total_size = static_cast<uint16_t>(4 + pos);
        uint16_t msg_type = 1022;
        uint8_t packet[256];
        memcpy(packet, &total_size, 2);
        memcpy(packet + 2, &msg_type, 2);
        memcpy(packet + 4, pb, pos);

        // Send via sub_1401DBA40(networkBuf, packetPtr, totalSize)
        // Get network buffer from send manager
        using GetMgr_t = uintptr_t(__fastcall*)();
        auto get_mgr = reinterpret_cast<GetMgr_t>(b + FN_GET_SEND_MGR);
        uintptr_t mgr = 0;
        __try { mgr = get_mgr(); } __except(EXCEPTION_EXECUTE_HANDLER) {}

        if (mgr) {
            // sendMgr+32 = network buffer pointer
            uintptr_t net_buf = 0;
            safe_read(mgr + 32, net_buf);

            if (net_buf && is_valid_ptr(net_buf)) {
                using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                auto raw_send = reinterpret_cast<RawSend_t>(b + 0x1DBA40);
                __try {
                    raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size));
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        break;
    }
    case ActionType::Revive: {
        // Send MsgAction with action=Reborn(25)
        // MsgActionPB: action(1), playerid(2), timestamp(3)
        // Packet: [u16 size][u16 type=1010][protobuf]
        uintptr_t b = get_base();
        uint32_t hero_id = 0;
        safe_read<uint32_t>(thisptr + OFF_ID, hero_id);

        auto write_varint = [](uint8_t* dst, uint32_t val) -> int {
            int n = 0;
            while (val > 0x7F) { dst[n++] = (val & 0x7F) | 0x80; val >>= 7; }
            dst[n++] = val & 0x7F;
            return n;
        };

        uint8_t pb[64];
        int pos = 0;
        // field 1: action = 25 (Reborn)
        pb[pos++] = 0x08; pos += write_varint(pb + pos, 25);
        // field 2: playerid
        pb[pos++] = 0x10; pos += write_varint(pb + pos, hero_id);
        // field 3: timestamp
        pb[pos++] = 0x18; pos += write_varint(pb + pos, static_cast<uint32_t>(GetTickCount()));

        uint16_t total_size = static_cast<uint16_t>(4 + pos);
        uint16_t msg_type = 1010; // MSGTYPE_ACTION
        uint8_t packet[128];
        memcpy(packet, &total_size, 2);
        memcpy(packet + 2, &msg_type, 2);
        memcpy(packet + 4, pb, pos);

        using GetMgr_t = uintptr_t(__fastcall*)();
        auto get_mgr = reinterpret_cast<GetMgr_t>(b + FN_GET_SEND_MGR);
        uintptr_t mgr = 0;
        __try { mgr = get_mgr(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (mgr) {
            uintptr_t net_buf = 0;
            safe_read(mgr + 32, net_buf);
            if (net_buf && is_valid_ptr(net_buf)) {
                using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                auto raw_send = reinterpret_cast<RawSend_t>(b + 0x1DBA40);
                __try { raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size)); }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        break;
    }
    case ActionType::UseItem: {
        using UseItem_t = void(__fastcall*)(uintptr_t, unsigned int);
        auto fn = reinterpret_cast<UseItem_t>(get_base() + FN_USE_ITEM);
        __try { fn(thisptr, pa.item_id); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    case ActionType::AnswerDialog: {
        using AnswerFn = void(__fastcall*)(uintptr_t, int, uintptr_t);
        auto fn = reinterpret_cast<AnswerFn>(get_base() + FN_DIALOG_ANSWER);
        __try { fn(thisptr, static_cast<int>(pa.target_id), 0); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    case ActionType::ActivateNpc: {
        using Fn = void(__fastcall*)(uintptr_t, uint32_t);
        auto fn = reinterpret_cast<Fn>(get_base() + FN_ACTIVATE_NPC);
        __try {
            fn(thisptr, pa.target_id);
            safe_write<uint32_t>(thisptr + OFF_NPC_ID, pa.target_id);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    case ActionType::AnswerNpc: {
        using Fn = void(__fastcall*)(uintptr_t, int, uintptr_t);
        auto fn = reinterpret_cast<Fn>(get_base() + FN_ANSWER_NPC);
        __try { fn(thisptr, static_cast<int>(pa.target_id), 0); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    case ActionType::DepositAll: {
        // Replicate the game's "Deposit All" VIP button exactly:
        // 1. Allocate protobuf object (0x40 bytes), set vtable, set field[5] = 2
        // 2. Use game's serializer to produce the wire-format
        // 3. Send via network singleton
        //
        // Old bot offsets -> new build mapping:
        //   old 0x317868 (operator new) -> standard operator new
        //   old 0x46C310 (protobuf vtable) -> 0x4721C0 (from disasm: off_1404721C0)
        //   old 0x24ACA0 (ByteSizeLong) -> 0x24EB60 (from disasm: sub_14024EB60)
        //   old 0x3178D0 (operator new) -> standard operator new
        //   old 0x3E6660 (memset) -> standard memset
        //   old 0x3517F0 (SerializeToArray) -> 0x354B40 (from disasm: sub_140354B40)
        //   old 0xBA470 (GetNetworkSingleton) -> 0x0B9490 (FN_GET_SEND_MGR)
        //   old 0x18B680 (Network::Send) -> 0x18EEA0 (FN_SEND_QUEUE)
        __try {
            uintptr_t b = get_base();

            // Allocate and init protobuf object (8 qwords = 0x40 bytes)
            uint64_t* pb = static_cast<uint64_t*>(malloc(0x40));
            if (!pb) break;
            memset(pb, 0, 0x40);
            pb[0] = b + 0x4721C0;  // vtable (off_1404721C0)
            pb[5] = 2;             // action = DepositAll (+0x28 = field 5)

            // Get serialized size
            using ByteSizeLong_t = uint64_t(__fastcall*)(uint64_t*);
            auto byte_size = reinterpret_cast<ByteSizeLong_t>(b + 0x24EB60);
            uint64_t size = byte_size(pb);

            if (size <= 0x8000 && size > 0) {
                // Allocate packet: 4 bytes header + body
                uint16_t* buf = static_cast<uint16_t*>(malloc(size + 4));
                if (buf) {
                    buf[0] = static_cast<uint16_t>(size + 4); // total size
                    buf[1] = 7015;                             // msg type 0x1B67
                    memset(buf + 2, 0, size);                  // zero body

                    // Serialize protobuf to body
                    using Serialize_t = bool(__fastcall*)(uint64_t*, uint8_t*, uint64_t);
                    auto serialize = reinterpret_cast<Serialize_t>(b + 0x354B40);
                    bool ok = serialize(pb, reinterpret_cast<uint8_t*>(buf + 2), size);

                    if (ok) {
                        // Send via network
                        using GetNet_t = uintptr_t(__fastcall*)();
                        auto get_net = reinterpret_cast<GetNet_t>(b + FN_GET_SEND_MGR);
                        uintptr_t net = get_net();
                        if (net) {
                            using SendFn = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                            auto send_fn = reinterpret_cast<SendFn>(b + FN_SEND_QUEUE);
                            send_fn(net, buf, static_cast<int64_t>(buf[0]));
                        }
                    }
                    free(buf);
                }
            }
            free(pb);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    case ActionType::VipTeleport: {
        __try {
            uintptr_t b = get_base();
            using VipTele_t = void(__fastcall*)(uintptr_t, int);
            auto vip_tele = reinterpret_cast<VipTele_t>(b + FN_VIP_TELEPORT);
            vip_tele(thisptr, pa.x);
            g_last_vip_teleport_time = static_cast<int64_t>(GetTickCount64());
            console::ok("[VIP] Teleported to city %d", pa.x);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            console::error("[VIP] Teleport crashed");
        }
        break;
    }
    case ActionType::WarehouseDeposit: {
        __try {
            uint32_t wh_id = 0;
            safe_read<uint32_t>(thisptr + OFF_WH_PKG_ID, wh_id);
            if (!wh_id) {
                console::warn("[WH] Warehouse not open");
                break;
            }
            uintptr_t b = get_base();
            using WhStore_t = void(__fastcall*)(uintptr_t, uint32_t);
            auto wh_store = reinterpret_cast<WhStore_t>(b + FN_WH_STORE);
            wh_store(thisptr, pa.target_id);
            console::ok("[WH] Deposited item %u to warehouse %u", pa.target_id, wh_id);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            console::error("[WH] Deposit crashed");
        }
        break;
    }
    default: break;
    }

}

inline bool install_process_hook() {
    uintptr_t hero = get_hero();
    if (!hero) return false;
    uintptr_t vtable_ptr = *reinterpret_cast<uintptr_t*>(hero);
    g_vtable_slot = reinterpret_cast<uintptr_t*>(vtable_ptr + VTABLE_PROCESS * 8);
    g_orig_process = reinterpret_cast<Process_t>(*g_vtable_slot);
    DWORD old = 0;
    VirtualProtect(g_vtable_slot, sizeof(uintptr_t), PAGE_READWRITE, &old);
    *g_vtable_slot = reinterpret_cast<uintptr_t>(hooked_process);
    VirtualProtect(g_vtable_slot, sizeof(uintptr_t), old, &old);
    return true;
}

inline void remove_process_hook() {
    if (g_vtable_slot && g_orig_process) {
        DWORD old = 0;
        VirtualProtect(g_vtable_slot, sizeof(uintptr_t), PAGE_READWRITE, &old);
        *g_vtable_slot = reinterpret_cast<uintptr_t>(g_orig_process);
        VirtualProtect(g_vtable_slot, sizeof(uintptr_t), old, &old);
        g_orig_process = nullptr;
        g_vtable_slot = nullptr;
    }
}

} // namespace game
