#include "console.h"
#include "game.h"
#include "field_scanner.h"
#include "entity_scanner.h"
#include "pathfinder.h"
#include "login.h"
#include "bot.h"
#include "scatter_lab.h"
#include "cross_map.h"
#include "auto_deposit.h"
#include "net_hook.h"
#include <filesystem>

// Wrappers so bot.h can call scatter_lab without circular includes
namespace bot {
bool scatter_bot_tick_wrapper(uintptr_t hero, int hx, int hy, int64_t now) {
    return scatter_lab::bot_scatter_tick(hero, hx, hy, now);
}
}

namespace bot {
int scatter_find_best_position_wrapper(
    int hx, int hy, double range, double half_angle, int max_jump,
    int& out_x, int& out_y) {
    auto monsters = scatter_lab::snapshot_monsters();
    auto result = scatter_lab::find_best_position(hx, hy, monsters, range, half_angle, max_jump);
    out_x = result.move_x;
    out_y = result.move_y;
    return result.predicted_hits;
}
} // namespace bot

// Pathfinder tick called from Process hook (game thread)
void pathfinder_tick_from_process(uintptr_t hero) {
    pathfinder::tick();
}
// Bot tick called from Process hook (game thread)
// Portal discovery result — set by game thread, consumed by network thread
struct PortalDiscovery {
    std::atomic<bool> ready{false};
    uint32_t src_map{0};
    int px{0}, py{0};
    std::string name;
    uint32_t dest_map{0};
    int dx{0}, dy{0};
};
static PortalDiscovery g_portal_discovery;

void bot_tick_from_process(uintptr_t hero) {
    // Player safety: handle disconnect requests (kills game process, GUI relaunches)
    if (player_safety::wants_disconnect()) {
        bot::stop();
        auto_deposit::stop();
        console::warn("[SAFETY] Disconnecting game client...");
        Sleep(500);
        ExitProcess(0);
        return;
    }

    // Portal exploration: check for map change after stepping on portal
    if (game::g_portal_explore.active) {
        uint32_t cur_map = game::get_map_id();
        if (game::g_portal_explore.phase == game::PortalExploreState::Stepping &&
            cur_map != game::g_portal_explore.source_map_id && cur_map != 0) {
            int hx = 0, hy = 0;
            game::safe_read<int>(hero + game::OFF_POS_X, hx);
            game::safe_read<int>(hero + game::OFF_POS_Y, hy);
            console::ok("[EXPLORE] Discovered: '%s' -> map %u at (%d,%d)",
                game::g_portal_explore.portal_name.c_str(), cur_map, hx, hy);

            // Stop pathfinding and invalidate grid since we're on a new map
            pathfinder::g_following.store(false);
            pathfinder::g_current_path.clear();
            pathfinder::invalidate_grid();

            // Save to Gateways.json in data dir (with dedup)
            {
                std::string gw_path = game::g_data_path.empty()
                    ? (game::g_client_path + "\\Gateways.json")
                    : (game::g_data_path + "\\Gateways.json");
                nlohmann::json arr = nlohmann::json::array();
                { std::ifstream f(gw_path); if (f.is_open()) { try { arr = nlohmann::json::parse(f); } catch (...) {} } }

                uint32_t src_map = game::g_portal_explore.source_map_id;
                int px = game::g_portal_explore.portal_x;
                int py = game::g_portal_explore.portal_y;

                // Update existing or add new
                bool updated = false;
                for (auto& e : arr) {
                    if (e.value("MapId", 0u) == src_map && e.value("X", -1) == px && e.value("Y", -1) == py) {
                        e["DestMapId"] = cur_map;
                        e["DestX"] = hx;
                        e["DestY"] = hy;
                        updated = true;
                        break;
                    }
                }
                if (!updated) {
                    nlohmann::json entry;
                    entry["MapId"] = src_map;
                    entry["X"] = px;
                    entry["Y"] = py;
                    entry["Type"] = 0;
                    entry["DestMapId"] = cur_map;
                    entry["DestX"] = hx;
                    entry["DestY"] = hy;
                    entry["NpcName"] = "";
                    arr.push_back(entry);
                }
                { std::ofstream f(gw_path); if (f.is_open()) f << arr.dump(2); }
                console::ok("[EXPLORE] Saved gateway to Gateways.json (%zu entries, %s)",
                    arr.size(), updated ? "updated" : "new");
            }

            // Queue discovery for GUI notification (consumed in on_message context)
            g_portal_discovery.src_map = game::g_portal_explore.source_map_id;
            g_portal_discovery.px = game::g_portal_explore.portal_x;
            g_portal_discovery.py = game::g_portal_explore.portal_y;
            g_portal_discovery.name = game::g_portal_explore.portal_name;
            g_portal_discovery.dest_map = cur_map;
            g_portal_discovery.dx = hx;
            g_portal_discovery.dy = hy;
            g_portal_discovery.ready.store(true);

            game::stop_portal_explore();
            game::explore_all_on_discovered();
        } else {
            game::portal_explore_tick(hero);
        }
        return; // Don't run bot/crossmap while exploring
    }

    // Explore-all drives the next portal exploration automatically
    if (game::g_explore_all.active) {
        game::explore_all_tick();
        // If explore_all just started a new portal explore, kick off pathfinding
        if (game::g_portal_explore.active &&
            game::g_portal_explore.phase == game::PortalExploreState::Walking &&
            !pathfinder::is_pathfinding()) {
            pathfinder::start_pathfind(game::g_portal_explore.portal_x, game::g_portal_explore.portal_y);
        }
        return; // Don't run bot while explore-all is active
    }

    // Auto-deposit: runs instead of bot when active
    if (auto_deposit::is_active()) {
        crossmap::tick(hero); // crossmap must tick for travel to work
        auto_deposit::tick(hero);
        return;
    }

    crossmap::tick(hero);
    bot::tick(hero);
    scatter_lab::tick();
}
#include "common/net/tcp_client.h"
#include "common/net/message.h"
#include <Windows.h>
#include <atomic>
#include <cmath>

using namespace ccbot::net;

static TcpClient g_client;
static std::atomic<bool> g_running{false};
static HANDLE g_thread{nullptr};
static constexpr uint16_t GUI_PORT = 1411;

// Build a state snapshot message with all hero info
static Message build_state_msg() {
    Message msg(MsgId::StateUpdate);
    // Pack all fields as a flat struct
    int32_t pos_x = 0, pos_y = 0;
    game::get_pos(pos_x, pos_y);

    msg << game::get_id();                              // uint32
    msg << pos_x << pos_y;                              // int, int
    msg << game::get_silver();                           // int64
    msg << game::get_stamina();                         // int
    msg << game::get_max_stamina();                     // int
    msg << game::get_pk_mode();                         // int
    // Read HP/MaxHP/MP via direct reads (no game function calls from background thread)
    int state_hp = 0, state_max_hp = 0, state_mp = 0;
    uintptr_t h_state = game::get_hero();
    if (h_state) {
        uintptr_t stats = 0;
        game::safe_read(h_state + game::OFF_STATS_PTR, stats);
        if (stats && game::is_valid_ptr(stats))
            game::safe_read<int>(stats + 8, state_hp);
        game::safe_read<int>(h_state + game::OFF_MAX_HP, state_max_hp);
        game::safe_read<int>(h_state + game::OFF_MP, state_mp);
    }
    msg << state_hp;                                    // int
    msg << state_max_hp;                                // int
    msg << state_mp;                                    // int
    msg << game::get_revive_countdown();                // int - seconds until can revive
    msg << game::get_cmd_type();                        // int
    msg << game::get_cmd_status();                      // int
    msg << static_cast<int64_t>(game::get_status_flag()); // int64

    // Booleans packed as uint16
    uint16_t flags = 0;
    if (game::is_dead())      flags |= (1 << 0);
    if (game::is_ghost())     flags |= (1 << 1);
    if (game::is_moving())    flags |= (1 << 2);
    if (game::is_jumping())   flags |= (1 << 3);
    if (game::is_attacking()) flags |= (1 << 4);
    if (game::is_acting())    flags |= (1 << 5);
    if (game::is_npc_active())flags |= (1 << 6);
    if (game::is_flying())    flags |= (1 << 7);
    if (game::is_invisible()) flags |= (1 << 8);
    if (game::is_poisoned())  flags |= (1 << 9);
    if (game::is_frozen())    flags |= (1 << 10);
    if (game::is_xp_full())   flags |= (1 << 11);
    if (game::is_red())       flags |= (1 << 12);
    if (game::is_black())     flags |= (1 << 13);
    if (game::is_vip())       flags |= (1 << 14);
    msg << flags;

    msg.push_string(game::get_name());

    // Extended fields (appended to preserve backward compat)
    msg << game::get_map_id();                          // uint32 - map ID
    int map_w = 0, map_h = 0;
    int world_w = 0, world_h = 0;
    {
        uintptr_t gm = game::get_base() + 0x4E02E0;
        game::safe_read<int>(gm + 0x30, map_w);
        game::safe_read<int>(gm + 0x34, map_h);
        game::safe_read<int>(gm + 0x38, world_w);  // 64 * map_w (full world pixel width)
        game::safe_read<int>(gm + 0x3C, world_h);  // world height / 2 from dmap
    }
    msg << map_w << map_h;                              // int, int - cell dimensions
    msg << world_w << world_h;                          // int, int - world pixel dimensions
    msg << game::get_level();                            // int
    return msg;
}

static void on_message(Message msg) {
    switch (msg.header.id) {
    case MsgId::Jump: {
        int x = 0, y = 0;
        if (msg.body.size() >= 8) { memcpy(&x, msg.body.data(), 4); memcpy(&y, msg.body.data() + 4, 4); }
        console::info("[CMD] Jump %d, %d", x, y);
        game::queue_jump(x, y);
        break;
    }
    case MsgId::Walk: {
        int x = 0, y = 0;
        if (msg.body.size() >= 8) { memcpy(&x, msg.body.data(), 4); memcpy(&y, msg.body.data() + 4, 4); }
        console::info("[CMD] Walk %d, %d", x, y);
        game::queue_walk(x, y);
        break;
    }
    case MsgId::Run: {
        int x = 0, y = 0;
        if (msg.body.size() >= 8) { memcpy(&x, msg.body.data(), 4); memcpy(&y, msg.body.data() + 4, 4); }
        console::info("[CMD] Run %d, %d", x, y);
        game::queue_run(x, y);
        break;
    }
    case MsgId::Attack: {
        uint32_t id = 0;
        if (msg.body.size() >= 4) memcpy(&id, msg.body.data(), 4);
        console::info("[CMD] Attack target %u", id);
        game::queue_attack(id);
        break;
    }
    case MsgId::PickUp: {
        uint32_t id = 0; int x = 0, y = 0;
        if (msg.body.size() >= 12) {
            memcpy(&id, msg.body.data(), 4);
            memcpy(&x, msg.body.data() + 4, 4);
            memcpy(&y, msg.body.data() + 8, 4);
        }
        console::info("[CMD] PickUp %u at %d,%d", id, x, y);
        game::queue_pickup(id, x, y);
        break;
    }
    case MsgId::MagicAttack: {
        uint32_t magic_id = 0;
        if (msg.body.size() >= 4) memcpy(&magic_id, msg.body.data(), 4);
        console::info("[CMD] MagicAttack (ground) id=%u", magic_id);
        game::queue_magic_attack(magic_id);
        break;
    }
    case MsgId::MagicAttackTarget: {
        uint32_t magic_id = 0, target_id = 0;
        if (msg.body.size() >= 8) {
            memcpy(&magic_id, msg.body.data(), 4);
            memcpy(&target_id, msg.body.data() + 4, 4);
        }
        console::info("[CMD] MagicAttack (target) magic=%u target=%u", magic_id, target_id);
        game::queue_magic_attack_target(magic_id, target_id);
        break;
    }
    case MsgId::Revive: {
        console::info("[CMD] Revive");
        game::queue_revive();
        break;
    }
    case MsgId::UseItem: {
        uint32_t item_id = 0;
        if (msg.body.size() >= 4) memcpy(&item_id, msg.body.data(), 4);
        // The UseItem function might need the instance ID from the inventory
        // Try looking up the item and using its instance ID
        console::info("[CMD] UseItem requested id=%u", item_id);

        // Check if this is an instance ID or type ID
        auto inv = game::get_inventory();
        bool found = false;
        for (auto& it : inv) {
            if (it.item_id == item_id) {
                console::info("[CMD] Found in inventory: instance=%u type=%u name=%s",
                    it.item_id, it.type_id, it.name);
                game::queue_use_item(it.item_id);
                found = true;
                break;
            }
        }
        if (!found) {
            console::warn("[CMD] Item %u not found in inventory, trying anyway", item_id);
            game::queue_use_item(item_id);
        }
        break;
    }
    case MsgId::ListInventory: {
        // Debug: dump inventory structure
        uintptr_t hero = game::get_hero();
        if (hero) {
            console::info("=== INVENTORY DEBUG ===");
            uintptr_t head = 0;
            uint64_t count = 0;
            game::safe_read(hero + 0xB50, head);
            game::safe_read(hero + 0xB58, count);
            console::info("head=0x%llX count=%llu", head, count);

            if (head && game::is_valid_ptr(head)) {
                // Dump first 3 nodes following the linked list
                uintptr_t node = head;
                for (int n = 0; n < 3 && node && game::is_valid_ptr(node); n++) {
                    console::info("  Node %d at 0x%llX:", n, node);
                    for (int off = 0; off < 48; off += 8) {
                        uintptr_t val = 0;
                        game::safe_read(node + off, val);
                        bool is_entity = false;
                        if (val && game::is_valid_ptr(val)) {
                            uintptr_t vt = 0;
                            game::safe_read(val, vt);
                            uintptr_t base = game::get_base();
                            if (vt > base + 0x400000 && vt < base + 0x500000) {
                                uint32_t eid = 0;
                                game::safe_read<uint32_t>(val + game::OFF_ID, eid);
                                // Dump first 64 bytes of the item object
                console::info("    [+%d] 0x%llX (ptr) vt=0x%llX ** ITEM OBJECT **", off, val, vt);
                for (int ioff = 0; ioff < 64; ioff += 8) {
                    uintptr_t iv = 0;
                    game::safe_read(val + ioff, iv);
                    uint32_t lo = (uint32_t)iv;
                    uint32_t hi = (uint32_t)(iv >> 32);
                    console::info("      item[+0x%02X] = 0x%llX (d32: %u, %u)", ioff, iv, lo, hi);
                }
                                is_entity = true;
                            }
                        }
                        if (!is_entity)
                            console::info("    [+%d] 0x%llX (%llu) %s", off, val, val,
                                (val && game::is_valid_ptr(val)) ? "(ptr)" : "");
                    }
                    // Follow next pointer
                    uintptr_t next = 0;
                    game::safe_read(node + 8, next);
                    if (!next || !game::is_valid_ptr(next) || next == node || next == head)
                        break;
                    node = next;
                }
            }
        }
        auto items = game::get_inventory();
        console::info("=== INVENTORY (%d items) ===", (int)items.size());
        for (auto& it : items)
            console::info("  id=%u", it.item_id);
        console::info("=== END ===");

        Message resp(MsgId::InventoryList);
        uint32_t ic = static_cast<uint32_t>(items.size());
        resp << ic;
        for (auto& it : items) {
            resp << it.item_id << it.type_id << it.amount << it.amount_limit;
            // Push name as 16 bytes
            size_t old = resp.body.size();
            resp.body.resize(old + 16, 0);
            memcpy(resp.body.data() + old, it.name, 16);
            resp.header.size = static_cast<uint32_t>(resp.body.size());
        }
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::SearchHp: {
        int target = 0;
        if (msg.body.size() >= 4) memcpy(&target, msg.body.data(), 4);
        uintptr_t hero = game::get_hero();
        if (hero) {
            console::info("=== HP SEARCH (value=%d) ===", target);

            // Search hero struct as int32
            for (int off = 0; off < 0x37B8; off += 4) {
                int32_t val = 0;
                if (game::safe_read<int32_t>(hero + off, val) && val == target)
                    console::ok("  hero+0x%04X int32 = %d", off, val);
            }

            // Also search as int16
            for (int off = 0; off < 0x37B8; off += 2) {
                int16_t val = 0;
                if (game::safe_read<int16_t>(hero + off, val) && val == target)
                    console::ok("  hero+0x%04X int16 = %d", off, val);
            }

            // Search the PlayerSet singleton too (HP might be in a stats subobject)
            uintptr_t base = game::get_base();
            uintptr_t ps = base + 0x4DF5F0;
            for (int off = 0; off < 1200; off += 4) {
                int32_t val = 0;
                if (game::safe_read<int32_t>(ps + off, val) && val == target)
                    console::ok("  PlayerSet+0x%04X int32 = %d", off, val);
            }

            // Also search pointers FROM the hero for nested objects
            // that might contain HP
            for (int hero_off = 0; hero_off < 0x37B8; hero_off += 8) {
                uintptr_t ptr = 0;
                if (!game::safe_read(hero + hero_off, ptr)) continue;
                if (!ptr || !game::is_valid_ptr(ptr)) continue;
                // Search first 256 bytes of the pointed object
                for (int off = 0; off < 256; off += 4) {
                    int32_t val = 0;
                    if (game::safe_read<int32_t>(ptr + off, val) && val == target) {
                        console::ok("  *(hero+0x%04X)+0x%02X int32 = %d (ptr=0x%llX)",
                            hero_off, off, val, ptr);
                    }
                }
            }

            console::info("=== END ===");
        }
        break;
    }
    case MsgId::RequestState: {
        if (g_client.is_connected())
            g_client.send(build_state_msg());
        break;
    }
    case MsgId::ListItems: {
        entities::debug_ground_items();
        entities::print_ground_items();
        auto items = entities::scan_ground_items();
        Message resp(MsgId::ItemList);
        uint32_t ic = static_cast<uint32_t>(items.size());
        resp << ic;
        for (auto& it : items)
            resp << it.item_id << it.type_id << it.x << it.y;
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::ListNearby: {
        entities::print_nearby();
        // Also send the list to GUI
        auto list = entities::scan_nearby();
        uint32_t hero_id = game::get_id();
        Message resp(MsgId::NearbyList);
        // Pack: count, then for each: id(4), x(4), y(4), type(4), role_kind(4), npc_sort(4), look_type(2), dead(1), name(16)
        uint32_t count = 0;
        for (auto& e : list) { if (e.id != hero_id) count++; }
        resp << count;
        for (auto& e : list) {
            if (e.id == hero_id) continue;
            resp << e.id << e.x << e.y << e.type << e.role_kind << e.npc_sort << e.look_type;
            uint8_t dead = e.is_dead ? 1 : 0;
            resp << dead;
            // Push name as fixed 16 bytes
            size_t old = resp.body.size();
            resp.body.resize(old + 16, 0);
            memcpy(resp.body.data() + old, e.name, 16);
            resp.header.size = static_cast<uint32_t>(resp.body.size());
        }
        if (g_client.is_connected())
            g_client.send(resp);
        break;
    }
    case MsgId::PathfindTo: {
        int x = 0, y = 0;
        if (msg.body.size() >= 8) {
            memcpy(&x, msg.body.data(), 4);
            memcpy(&y, msg.body.data() + 4, 4);
        }
        console::info("[CMD] Pathfind to %d,%d", x, y);
        pathfinder::start_pathfind(x, y);
        break;
    }
    case MsgId::PathfindStop: {
        console::info("[CMD] Stop pathfinding");
        pathfinder::stop_pathfind();
        break;
    }
    case MsgId::DebugCells: {
        pathfinder::debug_cells();
        break;
    }
    case MsgId::ScanAll: {
        scanner::scan_all();
        // Also dump GameMap for map ID discovery
        uintptr_t gm = game::get_base() + entities::OFF_GAMEMAP;
        console::info("=== GAMEMAP DUMP (base+0x4E02E0 = 0x%llX) ===", gm);
        for (int off = 0; off < 0x600; off += 4) {
            uint32_t val = 0;
            game::safe_read<uint32_t>(gm + off, val);
            if (val == 0) continue;
            const char* note = "";
            if (off == 0x30) note = " <-- width";
            else if (off == 0x34) note = " <-- height";
            console::info("  [+0x%03X] %10u (0x%08X)%s", off, val, val, note);
        }
        console::info("=== END GAMEMAP ===");

        // Scan both hero AND GameMap for circular deque patterns
        // Dump first 3 ground items raw memory
        {
        uintptr_t gm_base = game::get_base() + entities::OFF_GAMEMAP;
        uintptr_t vb = 0, ve = 0;
        game::safe_read(gm_base + entities::OFF_ITEM_VEC_BEGIN, vb);
        game::safe_read(gm_base + entities::OFF_ITEM_VEC_END, ve);
        size_t ic = (vb && ve > vb) ? (ve - vb) / 16 : 0;
        console::info("=== GROUND ITEM HEX DUMP (%zu items) ===", ic);
        for (size_t i = 0; i < ic && i < 5; i++) {
            uintptr_t item_ptr = 0;
            game::safe_read(vb + i * 16, item_ptr);
            if (!item_ptr) continue;
            uint32_t tid = 0;
            game::safe_read<uint32_t>(item_ptr + 0x04, tid);
            console::info("Item %zu (ptr=0x%llX type=%u):", i, item_ptr, tid);
            // Hex dump of node: every byte, 16 per line
            for (int row = 0; row < 3; row++) {
                uint8_t bytes[16];
                for (int b = 0; b < 16; b++) {
                    game::safe_read<uint8_t>(item_ptr + row * 16 + b, bytes[b]);
                }
                console::info("  +%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    row * 16,
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
            }
        }
        console::info("=== END GROUND ITEM DUMP ===");
        }
        console::info("=== VECTOR/DEQUE SCAN ===");
        // Also scan for std::vector pattern: [begin_ptr, end_ptr, capacity_ptr]
        // where (end-begin)/element_size gives a reasonable count
        {
        uintptr_t h = game::get_hero();
        if (h) {
            // Check hero+0x1970 area specifically (found count=200)
            for (int test_off = 0x1960; test_off <= 0x1990; test_off += 8) {
                uintptr_t v0 = 0, v1 = 0, v2 = 0;
                game::safe_read(h + test_off, v0);
                game::safe_read(h + test_off + 8, v1);
                game::safe_read(h + test_off + 16, v2);
                console::info("  hero+0x%03X: 0x%llX  0x%llX  0x%llX", test_off, v0, v1, v2);
                if (v0 && game::is_valid_ptr(v0) && v1 > v0 && v1 < v0 + 0x100000) {
                    size_t byte_count = v1 - v0;
                    // Try element sizes 8, 16, 24
                    for (int esize : {8, 16, 24}) {
                        if (byte_count % esize == 0) {
                            size_t count = byte_count / esize;
                            if (count > 0 && count < 1000) {
                                console::ok("    ^ VECTOR? elem_size=%d count=%zu", esize, count);
                                // Dump first 3 entries
                                for (size_t ei = 0; ei < 3 && ei < count; ei++) {
                                    console::info("    Entry %zu:", ei);
                                    for (int eoff = 0; eoff < esize && eoff < 32; eoff += 4) {
                                        uint32_t val = 0;
                                        game::safe_read<uint32_t>(v0 + ei * esize + eoff, val);
                                        if (val) console::info("      [+0x%02X] %u (0x%X)", eoff, val, val);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        }
        {
        uintptr_t scan_bases[2] = { game::get_hero(), game::get_base() + 0x4E02E0 };
        const char* scan_names[2] = { "hero", "GameMap" };
        int scan_starts[2] = { 0xD00, 0x200 };
        int scan_ends[2] = { 0x3700, 0x800 };
        for (int si = 0; si < 2; si++) {
        if (!scan_bases[si]) continue;
        for (int off = scan_starts[si]; off < scan_ends[si]; off += 8) {
                uintptr_t buf = 0;
                uint64_t mask = 0, head = 0, count = 0;
                game::safe_read(scan_bases[si] + off, buf);
                game::safe_read(scan_bases[si] + off + 8, mask);
                game::safe_read(scan_bases[si] + off + 16, head);
                game::safe_read(scan_bases[si] + off + 24, count);

                // Check deque pattern: buf is valid ptr, mask is power_of_2 - 1,
                // count > 0 and < 200
                if (!buf || !game::is_valid_ptr(buf)) continue;
                if (count == 0 || count > 200) continue;
                // mask should be 2^n - 1 (all bits set up to some point)
                uint64_t mask_plus_1 = mask + 1;
                if (mask_plus_1 == 0 || (mask_plus_1 & (mask_plus_1 - 1)) != 0) continue;

                console::ok("  DEQUE %s+0x%03X: buf=0x%llX mask=%llu head=%llu count=%llu",
                    scan_names[si], off, buf, mask, head, count);

                // Try to read first entry
                uintptr_t node = 0;
                game::safe_read(buf + (mask & head) * 8, node);
                if (node && game::is_valid_ptr(node)) {
                    uintptr_t ent = 0;
                    game::safe_read(node, ent);
                    if (ent && game::is_valid_ptr(ent)) {
                        // Dump first 0x30 bytes of first entry
                        console::info("    First entry ptr=0x%llX:", ent);
                        for (int eoff = 0; eoff < 0x30; eoff += 4) {
                            uint32_t v = 0;
                            game::safe_read<uint32_t>(ent + eoff, v);
                            if (v) console::info("      [+0x%02X] %u (0x%X)", eoff, v, v);
                        }
                    }
                }
            }
        }
        } // scope
        console::info("=== END DEQUE SCAN ===");
        break;
    }
    case MsgId::SetProxyPort: {
        uint16_t lp = 0;
        if (msg.body.size() >= 2) memcpy(&lp, msg.body.data(), 2);
        if (lp > 0) {
            net_hook::g_proxy_port.store(lp);
            console::ok("[CMD] Proxy port set to %u", lp);
        }
        break;
    }
    case MsgId::Login: {
        size_t off = 0;
        std::string username = msg.pop_string(off);
        std::string password = msg.pop_string(off);
        int server_idx = 0;
        if (off + 4 <= msg.body.size())
            memcpy(&server_idx, msg.body.data() + off, 4);

        login::queue_login(username.c_str(), password.c_str(), server_idx);
        break;
    }
    case MsgId::BotStart: {
        // GUI sends: config_path (string) + itemtype_path (string)
        size_t off = 0;
        std::string cfg_path = msg.pop_string(off);
        std::string item_path = msg.pop_string(off);
        console::info("[BOT] Loading config from: %s", cfg_path.c_str());
        console::info("[BOT] Loading items from: %s", item_path.c_str());
        // Derive client path from itemtype path (strip \ini\itemtype.json)
        auto sep = item_path.rfind("\\ini\\");
        if (sep == std::string::npos) sep = item_path.rfind("/ini/");
        if (sep != std::string::npos)
            game::g_client_path = item_path.substr(0, sep);
        bot::load_and_start(cfg_path, item_path);
        break;
    }
    case MsgId::BotStop: {
        bot::stop();
        break;
    }
    case MsgId::ScatterTest: {
        int tx = 0, ty = 0;
        if (msg.body.size() >= 8) {
            memcpy(&tx, msg.body.data(), 4);
            memcpy(&ty, msg.body.data() + 4, 4);
        }
        scatter_lab::begin_test(tx, ty);
        break;
    }
    case MsgId::DumpEntity: {
        uint32_t eid = 0;
        int32_t sval = -1;
        if (msg.body.size() >= 4) memcpy(&eid, msg.body.data(), 4);
        if (msg.body.size() >= 8) memcpy(&sval, msg.body.data() + 4, 4);
        entities::dump_entity_by_id(eid, sval);
        break;
    }
    case MsgId::SearchInt: {
        int target = 0;
        if (msg.body.size() >= 4) memcpy(&target, msg.body.data(), 4);
        scanner::search_int(target);
        break;
    }
    case MsgId::SearchString: {
        size_t off = 0;
        std::string target = msg.pop_string(off);
        scanner::search_string(target.c_str());
        break;
    }
    case MsgId::ActivateNpc: {
        if (msg.body.size() >= 4) {
            uint32_t npc_id = 0;
            memcpy(&npc_id, msg.body.data(), 4);
            console::info("[CMD] ActivateNpc id=%u", npc_id);
            game::queue_activate_npc(npc_id);
        }
        break;
    }
    case MsgId::DepositAll: {
        console::info("[CMD] VIP Deposit All");
        game::queue_deposit_all();
        break;
    }
    case MsgId::VipTeleport: {
        if (msg.body.size() >= 4) {
            int32_t city = 0;
            memcpy(&city, msg.body.data(), 4);
            int64_t now = static_cast<int64_t>(GetTickCount64());
            int64_t cd = game::g_last_vip_teleport_time + game::VIP_TELEPORT_COOLDOWN_MS - now;
            if (cd > 0) {
                console::warn("[VIP] Teleport on cooldown (%lld sec remaining)", cd / 1000);
            } else {
                static const char* names[] = {"?", "TC", "PC", "AC", "DC", "BI"};
                console::info("[CMD] VIP Teleport to %s (%d)", city >= 1 && city <= 5 ? names[city] : "?", city);
                game::queue_vip_teleport(city);
            }
        }
        break;
    }
    case MsgId::CrossMapInit: {
        size_t off = 0;
        std::string data_path = msg.pop_string(off);
        game::g_data_path = data_path; // Store for Gateways.json save/load
        crossmap::load_gateway_db(data_path);
        // Send map list back
        auto maps = crossmap::get_all_maps();
        Message resp(MsgId::CrossMapMaps);
        uint32_t count = static_cast<uint32_t>(maps.size());
        resp << count;
        for (auto& [id, name] : maps) {
            resp << id;
            resp.push_string(name);
        }
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::CrossMapTravel: {
        if (msg.body.size() >= 4) {
            uint32_t dest_map = 0;
            memcpy(&dest_map, msg.body.data(), 4);
            console::info("[CMD] Cross-map travel to %s(%u)", crossmap::map_name(dest_map), dest_map);
            crossmap::start_travel(dest_map);
        }
        break;
    }
    case MsgId::CrossMapStop: {
        crossmap::stop_travel();
        console::info("[CMD] Cross-map travel stopped");
        break;
    }
    case MsgId::DialogOptions: {
        // Read and send back current dialog state
        bool open = game::is_dialog_open();
        uintptr_t h = game::get_hero();
        if (h) {
            uintptr_t dbuf = 0, dmask = 0, dhead = 0, dcount = 0;
            game::safe_read(h + game::OFF_DIALOG_BUF, dbuf);
            game::safe_read(h + game::OFF_DIALOG_MASK, dmask);
            game::safe_read(h + game::OFF_DIALOG_HEAD, dhead);
            game::safe_read(h + game::OFF_DIALOG_COUNT, dcount);
            console::info("[DIALOG] open=%d buf=0x%llX mask=%llu head=%llu count=%llu",
                open ? 1 : 0, dbuf, dmask, dhead, dcount);
            // Dump first few raw entries
            if (dbuf && game::is_valid_ptr(dbuf) && dcount > 0) {
                uint64_t amask = dmask - 1;
                for (uint64_t di = 0; di < dcount && di < 8; di++) {
                    uint64_t didx = (dhead + di) & amask;
                    uintptr_t wrapper = 0;
                    game::safe_read(dbuf + didx * 8, wrapper);
                    uintptr_t inner = 0;
                    if (wrapper && game::is_valid_ptr(wrapper))
                        game::safe_read(wrapper, inner);
                    console::info("  [%llu] idx=%llu wrapper=0x%llX inner=0x%llX", di, didx, wrapper, inner);
                    if (inner && game::is_valid_ptr(inner)) {
                        // Dump 0x30 bytes of inner
                        uint8_t raw[0x30]{};
                        memcpy(raw, reinterpret_cast<void*>(inner), 0x30);
                        console::info("    +00: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                            raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],
                            raw[8],raw[9],raw[10],raw[11],raw[12],raw[13],raw[14],raw[15]);
                        console::info("    +10: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                            raw[16],raw[17],raw[18],raw[19],raw[20],raw[21],raw[22],raw[23],
                            raw[24],raw[25],raw[26],raw[27],raw[28],raw[29],raw[30],raw[31]);
                        console::info("    +20: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                            raw[32],raw[33],raw[34],raw[35],raw[36],raw[37],raw[38],raw[39],
                            raw[40],raw[41],raw[42],raw[43],raw[44],raw[45],raw[46],raw[47]);
                    }
                }
            }
        }
        auto opts = game::get_dialog_options();
        console::info("=== DIALOG (open=%d, options=%d) ===", open ? 1 : 0, (int)opts.size());
        for (auto& o : opts) {
            console::info("  [%d] type=%d id=%d text=\"%s\"",
                (int)(&o - &opts[0]), o.type, o.id, o.text.c_str());
        }
        console::info("=== END DIALOG ===");

        Message resp(MsgId::DialogOptions);
        uint8_t is_open = open ? 1 : 0;
        uint32_t count = static_cast<uint32_t>(opts.size());
        resp << is_open << count;
        for (auto& o : opts) {
            resp << static_cast<int32_t>(o.type) << static_cast<int32_t>(o.id);
            resp.push_string(o.text);
        }
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::DialogAnswer: {
        if (msg.body.size() >= 4) {
            int32_t option_id = 0;
            memcpy(&option_id, msg.body.data(), 4);
            console::info("[CMD] Answer dialog option=%d", option_id);
            game::queue_answer_dialog(option_id);
        }
        break;
    }
    case MsgId::PortalList: {
        uint32_t map_id = game::get_map_id();
        // Derive client path from game exe location if not set
        // exe is at <client>/bin/64/ImConquer.exe — go up 2 dirs
        if (game::g_client_path.empty()) {
            char exe_path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
            std::filesystem::path ep(exe_path);
            game::g_client_path = ep.parent_path().parent_path().parent_path().string();
        }
        if (!game::g_client_path.empty()) {
            game::load_portals_for_map(map_id, game::g_client_path);
        }
        console::info("=== PORTALS (map %u, %d portals, path=%s) ===",
            map_id, (int)game::g_map_portals.size(), game::g_client_path.c_str());
        for (auto& p : game::g_map_portals) {
            if (p.dest_map_id)
                console::info("  %s (%d,%d) -> map %u (%d,%d) [%s]",
                    p.name.c_str(), p.x, p.y, p.dest_map_id, p.dest_x, p.dest_y, p.type.c_str());
            else
                console::info("  %s (%d,%d) [%s] (dest unknown)", p.name.c_str(), p.x, p.y, p.type.c_str());
        }
        console::info("=== END PORTALS ===");

        Message resp(MsgId::PortalList);
        uint32_t count = static_cast<uint32_t>(game::g_map_portals.size());
        resp << map_id << count;
        for (auto& p : game::g_map_portals) {
            resp << static_cast<int32_t>(p.x) << static_cast<int32_t>(p.y);
            resp << p.dest_map_id << static_cast<int32_t>(p.dest_x) << static_cast<int32_t>(p.dest_y);
            resp.push_string(p.name);
            resp.push_string(p.type);
        }
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::PortalGo: {
        if (msg.body.size() >= 4) {
            int32_t idx = 0;
            memcpy(&idx, msg.body.data(), 4);
            if (idx >= 0 && idx < (int)game::g_map_portals.size()) {
                auto& p = game::g_map_portals[idx];
                console::info("[CMD] Pathfind to portal: %s (%d,%d)", p.name.c_str(), p.x, p.y);
                pathfinder::start_pathfind(p.x, p.y);
            }
        }
        break;
    }
    case MsgId::PortalExplore: {
        if (msg.body.size() >= 4) {
            int32_t idx = 0;
            memcpy(&idx, msg.body.data(), 4);
            if (idx >= 0 && idx < (int)game::g_map_portals.size()) {
                auto& p = game::g_map_portals[idx];
                console::info("[CMD] Explore portal: %s (%d,%d)", p.name.c_str(), p.x, p.y);
                game::start_portal_explore(idx);
                pathfinder::start_pathfind(p.x, p.y);
            }
        }
        break;
    }
    case MsgId::WarehouseDeposit: {
        if (msg.body.size() >= 4) {
            uint32_t item_id = 0;
            memcpy(&item_id, msg.body.data(), 4);
            if (game::is_warehouse_open()) {
                console::info("[CMD] Warehouse deposit item %u", item_id);
                game::queue_warehouse_deposit(item_id);
            } else {
                console::warn("[CMD] Warehouse not open, can't deposit");
            }
        }
        break;
    }
    case MsgId::WarehouseStatus: {
        // GUI requesting warehouse status
        Message resp(MsgId::WarehouseStatus);
        uint8_t open = game::is_warehouse_open() ? 1 : 0;
        uint32_t pkg_id = game::get_warehouse_id();
        resp << open << pkg_id;
        if (g_client.is_connected()) g_client.send(resp);
        break;
    }
    case MsgId::DepositNow: {
        console::info("[CMD] Force Deposit Now");
        auto_deposit::start(39);
        break;
    }
    case MsgId::StopDeposit: {
        console::info("[CMD] Stop Deposit");
        auto_deposit::stop();
        break;
    }
    case MsgId::ExploreAll: {
        console::info("[CMD] Explore All Portals");
        game::start_explore_all();
        break;
    }
    case MsgId::StopExploreAll: {
        console::info("[CMD] Stop Explore All");
        game::stop_explore_all();
        pathfinder::stop_pathfind();
        break;
    }
    case MsgId::NetControl: {
        // Proxy mode — DLL-side control not needed, GUI handles it
        break;
    }
    case MsgId::NetInject: {
        break;
    }
    case MsgId::Shutdown: {
        console::info("[CMD] Shutdown requested");
        bot::stop();
        g_running = false;
        break;
    }
    default:
        console::info("Unknown msg id=%u", static_cast<uint16_t>(msg.header.id));
        break;
    }
}

static void on_disconnect() { console::warn("GUI disconnected"); }

static DWORD WINAPI main_thread(LPVOID) {
    console::create("D3D11 Config");
    console::info("DLL loaded (PID: %u, base: 0x%llX)", GetCurrentProcessId(), game::get_base());

    // Install network packet hooks early (before game connects to server)
    net_hook::install();

    g_client.on_message = on_message;
    g_client.on_disconnect = on_disconnect;

    for (int i = 0; i < 10 && g_running; ++i) {
        console::info("Connecting to GUI (%d/10)...", i + 1);
        if (g_client.connect("127.0.0.1", GUI_PORT)) {
            console::ok("Connected to GUI");
            Message hs(MsgId::Handshake);
            hs.push_string("D3D11 Config DLL v2.0");
            g_client.send(hs);
            break;
        }
        Sleep(1000);
    }

    if (!g_client.is_connected())
        console::warn("No GUI - running standalone");

    console::info("Waiting for hero (auto-login active if credentials provided)...");
    bool hero_found = false;
    int login_attempts = 0;
    while (g_running && !hero_found) {
        uintptr_t hero = game::get_hero();
        if (hero) {
            uintptr_t vt = 0;
            uint32_t id = game::get_id();
            if (game::safe_read(hero, vt) && game::is_valid_ptr(vt) && id >= 1000000) {
                console::ok("Hero: 0x%llX [%s] ID=%u", hero, game::get_name().c_str(), id);
                hero_found = true;
            }
        }
        if (!hero_found) {
            if (login::g_login_pending.load() && !login::g_login_done.load()) {
                login::try_auto_login();
            }
            Sleep(2000);
        }
    }

    if (!hero_found) { g_client.disconnect(); console::destroy(); return 0; }


    if (game::install_process_hook())
        console::ok("Process hook installed");
    else
        console::error("Process hook FAILED");



    // Init scatter lab with log path in current working directory
    {
        char cwd[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        scatter_lab::init(std::string(cwd) + "\\scatter_log.json");
        entities::g_dump_path = std::string(cwd) + "\\npc_dumps.json";
    }

    console::ok("Running");

    // Change-based state tracking - only send when values differ
    struct LastState {
        int x{0}, y{0};
        int hp{0}, max_hp{0}, mp{0};
        int stamina{0}, max_stamina{0};
        int pk_mode{0};
        int cmd_type{0}, cmd_status{0};
        int64_t status_flag{0};
        int64_t silver{0};
        std::string name;
    } last;

    while (g_running) {
        bool changed = false;

        int x = 0, y = 0;
        game::get_pos(x, y);
        if (x != last.x || y != last.y) { last.x = x; last.y = y; changed = true; }

        // Read HP/MaxHP/MP via safe_read only (no game function calls from background thread)
        uintptr_t hero_bg = game::get_hero();
        int hp = 0, max_hp = 0, mp = 0;
        if (hero_bg) {
            // HP: read from stats sub-object
            uintptr_t stats = 0;
            game::safe_read(hero_bg + game::OFF_STATS_PTR, stats);
            if (stats && game::is_valid_ptr(stats))
                game::safe_read<int>(stats + 8, hp); // stats+8 is current HP
            // MaxHP: direct read
            game::safe_read<int>(hero_bg + game::OFF_MAX_HP, max_hp);
            // MP: direct read
            game::safe_read<int>(hero_bg + game::OFF_MP, mp);
        }
        if (hp != last.hp || max_hp != last.max_hp || mp != last.mp) {
            last.hp = hp; last.max_hp = max_hp; last.mp = mp; changed = true;
        }

        int sta = game::get_stamina();
        int max_sta = game::get_max_stamina();
        if (sta != last.stamina || max_sta != last.max_stamina) {
            last.stamina = sta; last.max_stamina = max_sta; changed = true;
        }

        int pk = game::get_pk_mode();
        if (pk != last.pk_mode) { last.pk_mode = pk; changed = true; }

        int ct = game::get_cmd_type();
        int cs = game::get_cmd_status();
        if (ct != last.cmd_type || cs != last.cmd_status) {
            last.cmd_type = ct; last.cmd_status = cs; changed = true;
        }

        int64_t sf = game::get_status_flag();
        if (sf != last.status_flag) {
            // Track death time - start timer when becoming ghost
            bool was_ghost = (last.status_flag & game::STATUS_GHOST) != 0;
            bool now_ghost = (sf & game::STATUS_GHOST) != 0;
            if (now_ghost && !was_ghost)
                game::g_death_time = static_cast<int64_t>(GetTickCount64());
            else if (!now_ghost && was_ghost)
                game::g_death_time = 0;

            last.status_flag = sf;
            changed = true;
        }

        int64_t silver = game::get_silver();
        if (silver != last.silver) { last.silver = silver; changed = true; }

        // Force update when ghost (for revive countdown)
        if (game::is_ghost()) changed = true;

        if (changed && g_client.is_connected())
            g_client.send(build_state_msg());

        // Bot and pathfinder run from Process hook (game thread)

        // Drain safety log queue and send to GUI
        if (g_client.is_connected()) {
            auto logs = player_safety::drain_logs();
            for (auto& text : logs) {
                Message lm(MsgId::Log);
                lm.push_string(text);
                g_client.send(lm);
            }
            // Send sound alert signal if requested
            if (player_safety::wants_sound()) {
                Message sm(MsgId::Log);
                sm.push_string("__SOUND_ALERT__");
                g_client.send(sm);
            }
        }

        // Check for portal discovery notification
        if (g_portal_discovery.ready.load() && g_client.is_connected()) {
            Message dm(MsgId::PortalDiscovered);
            dm << g_portal_discovery.src_map;
            dm << g_portal_discovery.px;
            dm << g_portal_discovery.py;
            dm.push_string(g_portal_discovery.name);
            dm << g_portal_discovery.dest_map;
            dm << g_portal_discovery.dx;
            dm << g_portal_discovery.dy;
            g_client.send(dm);
            g_portal_discovery.ready.store(false);
        }

        Sleep(50);
    }

    game::remove_process_hook();
    g_client.disconnect();
    console::info("Exiting");
    console::destroy();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_running = true;
        g_thread = CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); }
    }
    return TRUE;
}
