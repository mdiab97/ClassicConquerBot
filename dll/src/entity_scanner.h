#pragma once
#include "game.h"
#include "console.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <set>
#include <fstream>

namespace entities {

struct EntityInfo {
    uint32_t id{0};
    char name[17]{};
    int x{0}, y{0};
    int type{0};          // OFF_TYPE: 6=Hero, 7=Player
    int role_kind{0};     // OFF_ROLE_KIND: 4=Monster, 5=Pet, 6=Guard, 8=Booth, 9=NPC
    int npc_sort{0};      // OFF_NPC_SORT: 0=None,1=Shop,2=Task,3=Warehouse,6=Forge
    uint16_t look_type{0}; // OFF_LOOK_TYPE: NPC/monster appearance ID
    int64_t status{0};
    bool is_dead{false};

    // Classification by ID range (primary) + role_kind (supplementary)
    bool is_npc()     const { return id >= 1 && id < 400000; }
    bool is_monster() const {
        if (id < 400000 || id >= 500000) return false;
        if (is_guard()) return false;
        return true;
    }
    bool is_guard() const {
        if (id >= 400000 && id < 500000)
            return strstr(name, "Guard") || strstr(name, "Patrol");
        return false;
    }
    bool is_pet()     const { return id >= 900000 && id < 1000000; }
    bool is_booth()   const { return role_kind == game::ROLE_KIND_BOOTH; }
    bool is_player()  const { return id >= 1000000; }
    bool is_hero()    const { return type == 6; }

    // NPC sort from game memory at OFF_NPC_SORT (+0x2B0)
    enum class NpcSort : int { None=0, Shop=1, Task=2, Warehouse=3, Forge=6 };

    NpcSort get_npc_sort() const { return static_cast<NpcSort>(npc_sort); }

    bool is_npc_shop()      const { return npc_sort == 1; }
    bool is_npc_task()      const { return npc_sort == 2; }
    bool is_npc_warehouse() const { return npc_sort == 3; }
    bool is_npc_forge()     const { return npc_sort == 6; }

    const char* npc_sort_str() const {
        if (!is_npc()) return "";
        switch (npc_sort) {
            case 1:  return "Shop";
            case 2:  return "Task";
            case 3:  return "Storage";
            case 6:  return "Forge";
            default: return "NPC";
        }
    }
};

// CGamePlayerSet singleton: sub_140069830 returns &qword_1404DF5F0
// Circular deque layout (from IDA):
//   v17 = sub_140069830()  // returns int64_t* pointing to base+0x4DF5F0
//   v17[2] = buffer   (offset +0x10) - array of 8-byte node pointers
//   v17[3] = capacity (offset +0x18) - ring buffer capacity (e.g. 64)
//   v17[4] = head     (offset +0x20) - start index
//   v17[5] = count    (offset +0x28) - number of elements
// Access: buffer[((capacity-1) & (head + i)) * 8] -> node
//   node[0] = CRole* (the entity)
//   node[1] = control_block*
constexpr uintptr_t OFF_PLAYERSET = 0x4DF5F0;

// CGameMap singleton: sub_14008C370 returns &qword_1404E02E0
// Item manager (std::vector<shared_ptr<MapItemNode>>) at GameMap + 0x170:
//   Called as: sub_140140990(GameMap_ptr + 46, ...) where GameMap_ptr is int64_t*
//   So offset = 46 * 8 = 0x170
//   a1[1] = vector begin (+0x178), a1[2] = vector end (+0x180), a1[3] = vector cap (+0x188)
//   Each element = 16 bytes (shared_ptr): [node_ptr(8), control_block(8)]
//   Node layout (0x30 = 48 bytes):
//     +0x00 = shared_ptr<C2DMapItem> ptr
//     +0x08 = shared_ptr<C2DMapItem> control
//     +0x10 = ItemId (uint32)
//     +0x14 = TypeId (uint32)
//     +0x18 = position or other data
constexpr uintptr_t OFF_GAMEMAP = 0x4E02E0;
constexpr int OFF_ITEM_VEC_BEGIN = 0x178;  // GameMap + 46*8 + 8
constexpr int OFF_ITEM_VEC_END   = 0x180;  // GameMap + 46*8 + 16

struct GroundItem {
    uint32_t item_id{0};
    uint32_t type_id{0};
    int x{0}, y{0};
    uint8_t plus{0};  // +value at node offset +0x28
};

inline std::vector<EntityInfo> scan_nearby() {
    std::vector<EntityInfo> result;
    uintptr_t base = game::get_base();
    uintptr_t ps = base + OFF_PLAYERSET;

    uintptr_t buffer = 0;
    uint64_t capacity = 0, head = 0, count = 0;
    game::safe_read(ps + 0x10, buffer);   // v17[2]
    game::safe_read(ps + 0x18, capacity); // v17[3]
    game::safe_read(ps + 0x20, head);     // v17[4]
    game::safe_read(ps + 0x28, count);    // v17[5]

    if (!buffer || !game::is_valid_ptr(buffer) || count == 0 || count > 500 || capacity == 0)
        return result;

    uint64_t mask = capacity - 1;
    std::set<uint32_t> seen;

    for (uint64_t i = 0; i < count; i++) {
        // Match the game's iteration: buffer + 8 * ((mask) & (head + i))
        uint64_t index = mask & (head + i);
        uintptr_t node_ptr = 0;
        game::safe_read(buffer + index * 8, node_ptr);
        if (!node_ptr || !game::is_valid_ptr(node_ptr)) continue;

        // node[0] = CRole*
        uintptr_t role_ptr = 0;
        game::safe_read(node_ptr, role_ptr);
        if (!role_ptr || !game::is_valid_ptr(role_ptr)) continue;

        uintptr_t vt = 0;
        if (!game::safe_read(role_ptr, vt)) continue;
        if (vt < base + 0x400000 || vt > base + 0x500000) continue;

        EntityInfo info;
        game::safe_read<uint32_t>(role_ptr + game::OFF_ID, info.id);
        if (info.id < 100000 || seen.count(info.id)) continue;
        seen.insert(info.id);

        game::safe_read<int>(role_ptr + game::OFF_POS_X, info.x);
        game::safe_read<int>(role_ptr + game::OFF_POS_Y, info.y);
        if (info.x <= 0 || info.x > 20000 || info.y <= 0 || info.y > 20000) continue;

        game::safe_read<int>(role_ptr + game::OFF_TYPE, info.type);
        game::safe_read<int>(role_ptr + game::OFF_ROLE_KIND, info.role_kind);
        game::safe_read<int>(role_ptr + game::OFF_NPC_SORT, info.npc_sort);
        game::safe_read<uint16_t>(role_ptr + game::OFF_LOOK_TYPE, info.look_type);
        game::safe_read<int64_t>(role_ptr + game::OFF_STATUS_FLAG, info.status);
        info.is_dead = (info.status & game::STATUS_DEAD) != 0;
        if (game::is_valid_ptr(role_ptr + game::OFF_NAME))
            memcpy(info.name, reinterpret_cast<void*>(role_ptr + game::OFF_NAME), 16);

        result.push_back(info);
    }

    return result;
}

inline void debug_playerset() {
    uintptr_t base = game::get_base();
    uintptr_t ps = base + OFF_PLAYERSET;

    uintptr_t buffer = 0;
    uint64_t capacity = 0, head = 0, count = 0;
    game::safe_read(ps + 0x10, buffer);
    game::safe_read(ps + 0x18, capacity);
    game::safe_read(ps + 0x20, head);
    game::safe_read(ps + 0x28, count);

    console::info("=== PLAYER SET DEBUG ===");
    console::info("buffer=0x%llX cap=%llu head=%llu count=%llu", buffer, capacity, head, count);

    if (!buffer || !game::is_valid_ptr(buffer) || count == 0) {
        console::warn("Empty or invalid");
        console::info("=== END ===");
        return;
    }

    uint64_t mask = capacity - 1;
    int dumped = 0;
    for (uint64_t i = 0; i < count && dumped < 5; i++) {
        uint64_t index = mask & (head + i);
        uintptr_t node_ptr = 0;
        game::safe_read(buffer + index * 8, node_ptr);
        if (!node_ptr || !game::is_valid_ptr(node_ptr)) continue;

        uintptr_t role_ptr = 0;
        game::safe_read(node_ptr, role_ptr);

        uintptr_t vt = 0;
        uint32_t eid = 0; int ex = 0, ey = 0, etype = 0;
        char ename[17]{};
        if (role_ptr && game::is_valid_ptr(role_ptr)) {
            game::safe_read(role_ptr, vt);
            game::safe_read<uint32_t>(role_ptr + game::OFF_ID, eid);
            game::safe_read<int>(role_ptr + game::OFF_POS_X, ex);
            game::safe_read<int>(role_ptr + game::OFF_POS_Y, ey);
            game::safe_read<int>(role_ptr + game::OFF_TYPE, etype);
            if (game::is_valid_ptr(role_ptr + game::OFF_NAME))
                memcpy(ename, reinterpret_cast<void*>(role_ptr + game::OFF_NAME), 16);
        }

        console::info("  [%llu] node=0x%llX role=0x%llX vt=0x%llX id=%u pos=%d,%d type=%d name=%s",
            i, node_ptr, role_ptr, vt, eid, ex, ey, etype, ename);
        dumped++;
    }
    console::info("=== END ===");
}

inline std::vector<GroundItem> scan_ground_items() {
    std::vector<GroundItem> result;
    uintptr_t base = game::get_base();
    uintptr_t gm = base + OFF_GAMEMAP;

    uintptr_t vec_begin = 0, vec_end = 0;
    game::safe_read(gm + OFF_ITEM_VEC_BEGIN, vec_begin);
    game::safe_read(gm + OFF_ITEM_VEC_END, vec_end);

    if (!vec_begin || !game::is_valid_ptr(vec_begin) || vec_end <= vec_begin)
        return result;

    size_t count = (vec_end - vec_begin) / 16;
    if (count > 500) return result;

    for (size_t i = 0; i < count; i++) {
        uintptr_t node_ptr = 0;
        game::safe_read(vec_begin + i * 16, node_ptr);
        if (!node_ptr || !game::is_valid_ptr(node_ptr)) continue;

        // Node layout (from debug dump):
        //   [+0x00] low32 = ItemId, high32 = TypeId
        //   [+0x08] low32 = X, high32 = Y
        //   [+0x10] = shared_ptr<C2DMapItem> ptr
        //   [+0x18] = shared_ptr<C2DMapItem> control
        GroundItem item;
        game::safe_read<uint32_t>(node_ptr + 0x00, item.item_id);
        game::safe_read<uint32_t>(node_ptr + 0x04, item.type_id);
        game::safe_read<int>(node_ptr + 0x08, item.x);
        game::safe_read<int>(node_ptr + 0x0C, item.y);
        // Plus is on the C2DMapItem object at offset 0x48 (72)
        uintptr_t c2d_ptr = 0;
        game::safe_read(node_ptr + 0x10, c2d_ptr);
        if (c2d_ptr && game::is_valid_ptr(c2d_ptr))
            game::safe_read<uint8_t>(c2d_ptr + 0x48, item.plus);

        if (item.item_id == 0) continue;
        if (item.x <= 0 || item.x > 20000 || item.y <= 0 || item.y > 20000) continue;

        result.push_back(item);
    }

    return result;
}

inline void debug_ground_items() {
    uintptr_t base = game::get_base();
    uintptr_t gm = base + OFF_GAMEMAP;

    uintptr_t vec_begin = 0, vec_end = 0;
    game::safe_read(gm + OFF_ITEM_VEC_BEGIN, vec_begin);
    game::safe_read(gm + OFF_ITEM_VEC_END, vec_end);

    size_t count = (vec_end > vec_begin) ? (vec_end - vec_begin) / 16 : 0;
    console::info("=== GROUND ITEMS DEBUG ===");
    console::info("GameMap at 0x%llX, vec begin=0x%llX end=0x%llX count=%d",
        gm, vec_begin, vec_end, (int)count);

    if (!vec_begin || !game::is_valid_ptr(vec_begin) || count == 0) {
        console::warn("No ground items");
        console::info("=== END ===");
        return;
    }

    int dumped = 0;
    for (size_t i = 0; i < count && dumped < 10; i++) {
        uintptr_t node_ptr = 0;
        game::safe_read(vec_begin + i * 16, node_ptr);
        if (!node_ptr || !game::is_valid_ptr(node_ptr)) continue;

        // Dump node raw data
        console::info("  [%d] node=0x%llX", (int)i, node_ptr);
        for (int off = 0; off < 0x30; off += 8) {
            uintptr_t val = 0;
            game::safe_read(node_ptr + off, val);
            // Also show as two DWORDs
            uint32_t lo = (uint32_t)val;
            uint32_t hi = (uint32_t)(val >> 32);
            console::info("    [+0x%02X] 0x%llX (d32: %u, %u) %s", off, val, lo, hi,
                (val && game::is_valid_ptr(val)) ? "(ptr)" : "");
        }
        dumped++;
    }
    console::info("=== END ===");
}

inline void print_ground_items() {
    auto items = scan_ground_items();
    int hero_x = 0, hero_y = 0;
    game::get_pos(hero_x, hero_y);

    console::info("=== GROUND ITEMS (%d found) ===", (int)items.size());
    for (auto& it : items) {
        double dist = std::sqrt(static_cast<double>(
            (it.x - hero_x) * (it.x - hero_x) + (it.y - hero_y) * (it.y - hero_y)));
        console::info("  id=%-10u type=%-10u +%-2u pos=%d,%d dist=%.0f", it.item_id, it.type_id, it.plus, it.x, it.y, dist);
    }
    console::info("=== END ===");
}

inline void print_nearby() {
    auto entities = scan_nearby();
    int hero_x = 0, hero_y = 0;
    game::get_pos(hero_x, hero_y);
    uint32_t hero_id = game::get_id();

    console::info("=== NEARBY ENTITIES (%d found) ===", (int)entities.size());
    for (auto& e : entities) {
        if (e.id == hero_id) continue;
        double dist = std::sqrt(static_cast<double>(
            (e.x - hero_x) * (e.x - hero_x) + (e.y - hero_y) * (e.y - hero_y)));
        const char* kind_str = "?";
        if (e.is_monster())     kind_str = "Monster";
        else if (e.is_guard())  kind_str = "Guard";
        else if (e.is_npc())    kind_str = "NPC";
        else if (e.is_pet())    kind_str = "Pet";
        else if (e.is_booth())  kind_str = "Booth";
        else if (e.is_hero())   kind_str = "Hero";
        else if (e.is_player()) kind_str = "Player";
        else                    kind_str = "Other";
        const char* sub = e.npc_sort_str();
        if (sub[0])
            console::info("  %-10u %-16s %3d,%-3d %-8s %-9s look=%-5u %-5s %.0f",
                e.id, e.name, e.x, e.y, kind_str, sub, e.look_type, e.is_dead ? "DEAD" : "alive", dist);
        else
            console::info("  %-10u %-16s %3d,%-3d %-8s look=%-5u %-5s %.0f",
                e.id, e.name, e.x, e.y, kind_str, e.look_type, e.is_dead ? "DEAD" : "alive", dist);
    }
    console::info("=== END ===");
}

inline std::string g_dump_path; // set on init

// Dump an entity's raw memory to npc_dumps.json with a user-assigned label
inline void dump_entity_by_id(uint32_t target_id, int label = 0) {
    uintptr_t base = game::get_base();
    uintptr_t ps = base + OFF_PLAYERSET;

    uintptr_t buffer = 0, capacity = 0, head = 0, count = 0;
    game::safe_read(ps + 0x10, buffer);
    game::safe_read(ps + 0x18, capacity);
    game::safe_read(ps + 0x20, head);
    game::safe_read(ps + 0x28, count);
    if (!buffer || count == 0 || capacity == 0) {
        console::error("PlayerSet empty");
        return;
    }

    uint64_t mask = capacity - 1;
    for (uint64_t i = 0; i < count; i++) {
        uintptr_t node_ptr = 0;
        game::safe_read(buffer + (mask & (head + i)) * 8, node_ptr);
        if (!node_ptr) continue;
        uintptr_t role = 0;
        game::safe_read(node_ptr, role);
        if (!role || !game::is_valid_ptr(role)) continue;

        uint32_t id = 0;
        game::safe_read<uint32_t>(role + game::OFF_ID, id);
        if (id != target_id) continue;

        char name[17]{};
        if (game::is_valid_ptr(role + game::OFF_NAME))
            memcpy(name, reinterpret_cast<void*>(role + game::OFF_NAME), 16);
        name[16] = 0;

        const char* sort_names[] = {"None","Shop","Task","Warehouse","Teleport","Pet","Forge"};
        const char* sort_str = (label >= 0 && label <= 6) ? sort_names[label] : "Unknown";
        console::ok("Dumping %s (ID=%u) label=%s", name, id, sort_str);

        // Read raw memory as uint32 array
        nlohmann::json entry;
        entry["id"] = id;
        entry["name"] = std::string(name);
        entry["label"] = label;
        entry["label_str"] = sort_str;
        entry["ptr"] = (uint64_t)role;

        nlohmann::json fields = nlohmann::json::object();
        for (int off = 0; off < 0x400; off += 4) {
            uint32_t val = 0;
            game::safe_read<uint32_t>(role + off, val);
            if (val != 0) {
                char key[16];
                snprintf(key, sizeof(key), "0x%03X", off);
                fields[key] = val;
            }
        }
        // Also read uint16 values at odd offsets for smaller fields
        nlohmann::json fields16 = nlohmann::json::object();
        for (int off = 0; off < 0x400; off += 2) {
            uint16_t val = 0;
            game::safe_read<uint16_t>(role + off, val);
            if (val != 0 && val < 10000) { // likely a type/sort value
                char key[16];
                snprintf(key, sizeof(key), "0x%03X", off);
                fields16[key] = val;
            }
        }
        entry["u32"] = fields;
        entry["u16_small"] = fields16;

        // Load existing file, append, save
        nlohmann::json all_dumps = nlohmann::json::array();
        if (!g_dump_path.empty()) {
            try {
                std::ifstream fin(g_dump_path);
                if (fin.is_open()) all_dumps = nlohmann::json::parse(fin);
            } catch (...) {}

            all_dumps.push_back(entry);

            try {
                std::ofstream fout(g_dump_path);
                fout << all_dumps.dump(2);
                console::ok("Saved dump #%d to %s", (int)all_dumps.size(), g_dump_path.c_str());
            } catch (...) {
                console::error("Failed to save dump file");
            }
        }
        return;
    }
    console::error("Entity %u not found", target_id);
}

} // namespace entities
