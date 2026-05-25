#pragma once
#include "game.h"
#include "pathfinder.h"
#include "entity_scanner.h"
#include "cross_map.h"
#include "console.h"

namespace auto_deposit {

constexpr uint32_t MARKET_MAP_ID = 1036;
constexpr int MARKET_X = 187, MARKET_Y = 185;  // walkable center spot
constexpr int MET_NPC_X = 181, MET_NPC_Y = 184;  // walkable near Conductress
constexpr int WH_NPC_X = 183, WH_NPC_Y = 181;    // walkable near Warehouseman

inline bool is_skip_item(uint32_t type_id) {
    if (type_id / 1000 == 1050) return true;           // arrows
    if (type_id / 10000 == 115) return true;            // potions 115xxxx
    if (type_id / 10000 == 100) return true;            // potions 100xxxx
    if (type_id >= 1060020 && type_id <= 1060029) return true; // teleport gates
    return false;
}

enum class Stage {
    Idle,
    TravelToMarket,       // use crossmap to get to market
    WaitForCrossmap,      // waiting for crossmap travel to complete
    ArrivedAtMarket,      // on market map, path to center
    DepositMets,
    DepositDbs,
    VipDepositAll,
    VipDepositAllWait,
    OpenWarehouse,
    DepositMoney,
    DepositItems,
    TravelBack,           // use crossmap to return
    WaitForReturn,
    Done,
};

struct State {
    bool active{false};
    Stage stage{Stage::Idle};
    int64_t next_action_time{0};
    uint32_t origin_map{0};
    int origin_x{0}, origin_y{0};
    int max_items{39};
    bool met_deposited{false};
    int deposit_count{0};
};

inline State g_state;

inline void start(int max_items = 39) {
    uintptr_t h = game::get_hero();
    if (!h) { console::warn("[DEPOSIT] No hero"); return; }
    auto& s = g_state;
    s.active = true;
    s.stage = Stage::TravelToMarket;
    s.next_action_time = 0;
    s.origin_map = game::get_map_id();
    game::get_pos(s.origin_x, s.origin_y);
    s.max_items = max_items;
    s.met_deposited = false;
    s.deposit_count = 0;
    console::ok("[DEPOSIT] Starting (return to map %u at %d,%d)", s.origin_map, s.origin_x, s.origin_y);
}

inline void stop() {
    g_state.active = false;
    g_state.stage = Stage::Idle;
    crossmap::stop_travel();
    pathfinder::stop_pathfind();
    console::ok("[DEPOSIT] Stopped (%d items deposited)", g_state.deposit_count);
}

inline bool is_active() { return g_state.active; }

// Close any open NPC dialog
inline void close_npc_dialog() {
    game::queue_answer_dialog(255);
}

// Find NPC by name in visible entities (no position requirement)
inline uint32_t find_visible_npc(const char* name) {
    auto nearby = entities::scan_nearby();
    for (auto& e : nearby) {
        if (strcmp(e.name, name) == 0) return e.id;
    }
    return 0;
}

// Try to interact with NPC by name. Returns true if NPC found and action taken.
// If NPC not visible, returns false (caller should move closer to expected area).
inline bool try_activate_npc(const char* name, int64_t now, int64_t& next_time) {
    uint32_t npc_id = find_visible_npc(name);
    if (!npc_id) return false;
    uint32_t active = game::get_active_npc();
    if (active != npc_id) {
        game::queue_activate_npc(npc_id);
        next_time = now + 1500;
    }
    return true;
}

inline bool has_meteors() {
    for (auto& it : game::get_inventory()) if (it.type_id == 1088001) return true;
    return false;
}

inline bool has_dragonballs() {
    for (auto& it : game::get_inventory()) if (it.type_id == 1088002) return true;
    return false;
}

inline uint32_t find_first_depositable() {
    for (auto& it : game::get_inventory()) if (!is_skip_item(it.type_id)) return it.item_id;
    return 0;
}

inline bool should_auto_deposit(int max_items) {
    return (int)game::get_inventory().size() >= max_items;
}

// Main tick
inline void tick(uintptr_t hero) {
    if (!g_state.active || !hero) return;
    int64_t now = static_cast<int64_t>(GetTickCount64());
    if (now < g_state.next_action_time) return;

    auto& s = g_state;
    int hx = 0, hy = 0;
    game::get_pos(hx, hy);
    uint32_t cur_map = game::get_map_id();

    switch (s.stage) {

    case Stage::TravelToMarket: {
        if (cur_map == MARKET_MAP_ID) {
            s.stage = Stage::ArrivedAtMarket;
            return;
        }
        if (!crossmap::g_db_loaded) {
            console::warn("[DEPOSIT] Gateway DB not loaded, can't travel");
            stop();
            return;
        }
        if (!crossmap::is_traveling()) {
            console::info("[DEPOSIT] Starting cross-map travel to Market (%u)", MARKET_MAP_ID);
            crossmap::start_travel(MARKET_MAP_ID, MARKET_X, MARKET_Y);
        }
        s.stage = Stage::WaitForCrossmap;
        s.next_action_time = now + 500;
        break;
    }

    case Stage::WaitForCrossmap: {
        if (cur_map == MARKET_MAP_ID) {
            console::ok("[DEPOSIT] Arrived at Market map");
            crossmap::stop_travel();
            s.stage = Stage::ArrivedAtMarket;
            s.next_action_time = now + 250;
            return;
        }
        if (!crossmap::is_traveling()) {
            console::warn("[DEPOSIT] Crossmap stopped, retrying travel to Market");
            s.stage = Stage::TravelToMarket;
            s.next_action_time = now + 1000;
        }
        break;
    }

    case Stage::ArrivedAtMarket: {
        bool can_see_npc = find_visible_npc("TreasureBank") ||
                           find_visible_npc("Warehouseman") ||
                           find_visible_npc("ComposeBank");
        if (can_see_npc) {
            console::ok("[DEPOSIT] Market NPCs in range");
            if (game::is_vip()) {
                s.stage = has_meteors() ? Stage::DepositMets :
                          has_dragonballs() ? Stage::DepositDbs : Stage::VipDepositAll;
                s.met_deposited = false;
            } else {
                s.stage = Stage::OpenWarehouse;
            }
            s.next_action_time = now + 250;
            return;
        }
        if (!pathfinder::is_pathfinding())
            pathfinder::start_pathfind(MARKET_X, MARKET_Y);
        break;
    }

    case Stage::DepositMets: {
        if (!has_meteors()) {
            s.stage = has_dragonballs() ? Stage::DepositDbs : Stage::VipDepositAll;
            s.next_action_time = now + 250;
            return;
        }
        uint32_t npc_id = find_visible_npc("TreasureBank");
        if (!npc_id) {
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(MARKET_X, MARKET_Y);
            s.next_action_time = now + 500;
            return;
        }
        if (!game::is_dialog_open()) {
            console::info("[DEPOSIT] Activating TreasureBank %u for meteors", npc_id);
            game::queue_activate_npc(npc_id);
            s.next_action_time = now + 800;
            return;
        }
        auto opts = game::get_dialog_options();
        if (opts.empty()) {
            s.next_action_time = now + 200;
            return;
        }
        for (auto& o : opts) {
            if (o.type == 1) {
                console::info("[DEPOSIT] Clicking met deposit: id=%d '%s'", o.id, o.text.c_str());
                game::queue_answer_dialog(o.id);
                s.stage = has_dragonballs() ? Stage::DepositDbs : Stage::VipDepositAll;
                s.next_action_time = now + 1000;
                break;
            }
        }
        break;
    }

    case Stage::DepositDbs: {
        if (!has_dragonballs()) {
            s.stage = Stage::VipDepositAll;
            s.next_action_time = now + 250;
            return;
        }
        uint32_t npc_id = find_visible_npc("TreasureBank");
        if (!npc_id) {
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(MARKET_X, MARKET_Y);
            s.next_action_time = now + 500;
            return;
        }
        if (!game::is_dialog_open()) {
            console::info("[DEPOSIT] Activating TreasureBank %u for DBs", npc_id);
            game::queue_activate_npc(npc_id);
            s.next_action_time = now + 800;
            return;
        }
        auto opts = game::get_dialog_options();
        if (opts.empty()) {
            s.next_action_time = now + 200;
            return;
        }
        // Find second button option (DBs)
        int btn_idx = 0;
        bool answered = false;
        for (auto& o : opts) {
            if (o.type == 1) {
                if (btn_idx == 1) {
                    console::info("[DEPOSIT] Clicking DB deposit: id=%d '%s'", o.id, o.text.c_str());
                    game::queue_answer_dialog(o.id);
                    answered = true;
                    break;
                }
                btn_idx++;
            }
        }
        if (answered) {
            s.stage = Stage::VipDepositAll;
            s.next_action_time = now + 1000;
        } else {
            // Not enough buttons yet — close and retry
            console::warn("[DEPOSIT] DB dialog: only %d buttons, retrying", btn_idx);
            close_npc_dialog();
            s.next_action_time = now + 500;
        }
        break;
    }

    case Stage::VipDepositAll: {
        console::info("[DEPOSIT] VIP Deposit All");
        game::queue_deposit_all();
        s.stage = Stage::VipDepositAllWait;
        s.next_action_time = now + 1000;
        break;
    }

    case Stage::VipDepositAllWait: {
        uint32_t item = find_first_depositable();
        if (item) {
            console::info("[DEPOSIT] Items remain, opening warehouse...");
            s.stage = Stage::OpenWarehouse;
        } else {
            console::ok("[DEPOSIT] All deposited!");
            s.stage = Stage::TravelBack;
        }
        s.next_action_time = now + 300;
        break;
    }

    case Stage::OpenWarehouse: {
        if (game::is_warehouse_open()) {
            console::info("[DEPOSIT] Warehouse confirmed open (pkg=%u)", game::get_warehouse_id());
            s.stage = Stage::DepositMoney;
            s.next_action_time = now + 500;
            return;
        }
        close_npc_dialog();
        uint32_t npc_id = find_visible_npc("Warehouseman");
        if (!npc_id) {
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(MARKET_X, MARKET_Y);
            s.next_action_time = now + 500;
            return;
        }
        console::info("[DEPOSIT] Activating Warehouseman %u", npc_id);
        game::queue_activate_npc(npc_id);
        s.next_action_time = now + 1500;
        break;
    }

    case Stage::DepositMoney: {
        int64_t silver = game::get_silver();
        if (silver > 150000) {
            console::info("[DEPOSIT] TODO: deposit %lld excess silver", silver - 120000);
            // Silver deposit not yet implemented via MsgItem
        }
        s.stage = Stage::DepositItems;
        s.next_action_time = now + 500;
        break;
    }

    case Stage::DepositItems: {
        if (!game::is_warehouse_open()) {
            s.stage = Stage::OpenWarehouse;
            s.next_action_time = now + 1000;
            return;
        }
        // Skip items we already tried to deposit (waiting for server to remove them)
        static uint32_t last_deposited_id = 0;
        static int same_item_count = 0;

        uint32_t item_id = find_first_depositable();
        if (!item_id) {
            console::ok("[DEPOSIT] All items deposited (%d total)", s.deposit_count);
            last_deposited_id = 0;
            same_item_count = 0;
            s.stage = Stage::TravelBack;
            s.next_action_time = now + 500;
            return;
        }

        // If same item keeps appearing, server hasn't processed it yet — wait longer
        if (item_id == last_deposited_id) {
            same_item_count++;
            if (same_item_count > 5) {
                // Skip this item, it might be un-depositable
                console::warn("[DEPOSIT] Item %u stuck, skipping", item_id);
                // Mark as skip by moving to next
                last_deposited_id = 0;
                same_item_count = 0;
                s.next_action_time = now + 1000;
                return;
            }
            s.next_action_time = now + 500; // wait for server to process
            return;
        }

        console::info("[DEPOSIT] Depositing item %u", item_id);
        game::queue_warehouse_deposit(item_id);
        last_deposited_id = item_id;
        same_item_count = 0;
        s.deposit_count++;
        s.next_action_time = now + 500; // wait for server response
        break;
    }

    case Stage::TravelBack: {
        if (cur_map == s.origin_map) {
            int d = std::max(std::abs(hx - s.origin_x), std::abs(hy - s.origin_y));
            if (d <= 5) {
                console::ok("[DEPOSIT] Back at hunting spot!");
                stop();
                return;
            }
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(s.origin_x, s.origin_y);
            return;
        }
        if (!crossmap::is_traveling()) {
            console::info("[DEPOSIT] Cross-map travel back to map %u", s.origin_map);
            crossmap::start_travel(s.origin_map, s.origin_x, s.origin_y);
        }
        s.stage = Stage::WaitForReturn;
        s.next_action_time = now + 500;
        break;
    }

    case Stage::WaitForReturn: {
        if (cur_map == s.origin_map) {
            crossmap::stop_travel();
            int d = std::max(std::abs(hx - s.origin_x), std::abs(hy - s.origin_y));
            if (d <= 5) {
                console::ok("[DEPOSIT] Back at hunting spot!");
                stop();
                return;
            }
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(s.origin_x, s.origin_y);
            s.stage = Stage::TravelBack;
            return;
        }
        if (!crossmap::is_traveling()) {
            console::warn("[DEPOSIT] Crossmap stopped, retrying return");
            s.stage = Stage::TravelBack;
            s.next_action_time = now + 1000;
        }
        break;
    }

    default: break;
    }
}

} // namespace auto_deposit
