#pragma once
#include "game.h"
#include "entity_scanner.h"
#include "pathfinder.h"
#include "console.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <functional>

namespace crossmap {

// ═══════════════════════════════════════════════════════════════════════
// GATEWAY DATABASE
// ═══════════════════════════════════════════════════════════════════════

enum class GatewayType { Portal = 0, Npc = 1 };

struct Gateway {
    uint32_t map_id{0};
    int x{0}, y{0};              // position of portal/NPC on source map
    GatewayType type{GatewayType::Portal};
    uint32_t dest_map_id{0};
    int dest_x{0}, dest_y{0};    // landing position on destination map
    // NPC-specific
    std::string npc_name;
    std::vector<uint32_t> options; // dialog option IDs to click in order
    uint32_t require_level_above{0};
    uint32_t require_level_below{0};
    uint32_t require_money{0};
    uint32_t require_profession{0};
};

inline std::unordered_map<uint32_t, std::vector<Gateway>> g_gateways; // map_id -> gateways
inline std::unordered_map<uint32_t, std::string> g_map_names;         // map_id -> name
inline bool g_db_loaded{false};
inline std::string g_db_path;

inline void load_gateway_db(const std::string& data_path) {
    g_gateways.clear();
    g_map_names.clear();
    g_db_loaded = false;
    g_db_path = data_path;

    // Load Gateways.json
    {
        std::ifstream f(data_path + "/Gateways.json");
        if (!f.is_open()) {
            // Try alternate path
            f.open(data_path + "\\Gateways.json");
        }
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                for (auto& e : j) {
                    Gateway g;
                    g.map_id = e.value("MapId", 0u);
                    g.x = e.value("X", 0);
                    g.y = e.value("Y", 0);
                    g.type = static_cast<GatewayType>(e.value("Type", 0));
                    g.dest_map_id = e.value("DestMapId", 0u);
                    g.dest_x = e.value("DestX", 0);
                    g.dest_y = e.value("DestY", 0);
                    g.npc_name = e.value("NpcName", "");
                    g.require_level_above = e.value("RequireLevelAbove", 0u);
                    g.require_level_below = e.value("RequireLevelBelow", 0u);
                    g.require_money = e.value("RequireMoney", 0u);
                    g.require_profession = e.value("RequireProfession", 0u);
                    if (e.contains("Options")) {
                        for (auto& o : e["Options"])
                            g.options.push_back(o.get<uint32_t>());
                    }
                    g_gateways[g.map_id].push_back(std::move(g));
                }
            } catch (const std::exception& ex) {
                console::error("[CROSSMAP] Gateways.json parse error: %s", ex.what());
            }
        } else {
            console::warn("[CROSSMAP] Gateways.json not found at %s", data_path.c_str());
        }
    }

    // Load MapName.json
    {
        std::ifstream f(data_path + "/MapName.json");
        if (!f.is_open()) f.open(data_path + "\\MapName.json");
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                for (auto& e : j) {
                    uint32_t id = e.value("MapId", 0u);
                    std::string name = e.value("MapName", "");
                    if (id) g_map_names[id] = name;
                }
            } catch (...) {}
        }
    }

    int total = 0;
    for (auto& kv : g_gateways) total += (int)kv.second.size();
    console::ok("[CROSSMAP] Loaded %d gateways across %d maps, %d map names",
        total, (int)g_gateways.size(), (int)g_map_names.size());
    g_db_loaded = true;
}

inline void save_gateway_db() {
    if (g_db_path.empty()) return;

    // Save Gateways.json
    nlohmann::json arr = nlohmann::json::array();
    for (auto& [map_id, gws] : g_gateways) {
        for (auto& gw : gws) {
            nlohmann::json j;
            j["map_id"] = gw.map_id;
            j["x"] = gw.x;
            j["y"] = gw.y;
            j["type"] = (gw.type == GatewayType::Portal) ? "portal" : "npc";
            j["dest_map_id"] = gw.dest_map_id;
            j["dest_x"] = gw.dest_x;
            j["dest_y"] = gw.dest_y;
            if (!gw.npc_name.empty()) j["npc_name"] = gw.npc_name;
            if (!gw.options.empty()) j["options"] = gw.options;
            if (gw.require_level_above) j["require_level_above"] = gw.require_level_above;
            if (gw.require_level_below) j["require_level_below"] = gw.require_level_below;
            if (gw.require_money) j["require_money"] = gw.require_money;
            if (gw.require_profession) j["require_profession"] = gw.require_profession;
            arr.push_back(j);
        }
    }
    std::ofstream f(g_db_path + "/Gateways.json");
    if (f.is_open()) {
        f << arr.dump(2);
        console::ok("[CROSSMAP] Saved %d gateways to Gateways.json", (int)arr.size());
    }

    // Save MapName.json
    nlohmann::json names;
    for (auto& [id, name] : g_map_names) names[std::to_string(id)] = name;
    std::ofstream nf(g_db_path + "/MapName.json");
    if (nf.is_open()) nf << names.dump(2);
}

// Add a discovered portal gateway to the DB
inline void add_discovered_portal(uint32_t src_map, int src_x, int src_y,
                                   uint32_t dest_map, int dest_x, int dest_y) {
    // Check if this gateway already exists
    auto& gws = g_gateways[src_map];
    for (auto& gw : gws) {
        if (gw.type == GatewayType::Portal && gw.x == src_x && gw.y == src_y) {
            gw.dest_map_id = dest_map;
            gw.dest_x = dest_x;
            gw.dest_y = dest_y;
            console::ok("[CROSSMAP] Updated portal (%d,%d) on map %u -> map %u (%d,%d)",
                src_x, src_y, src_map, dest_map, dest_x, dest_y);
            save_gateway_db();
            return;
        }
    }
    // New gateway
    Gateway gw;
    gw.map_id = src_map;
    gw.x = src_x;
    gw.y = src_y;
    gw.type = GatewayType::Portal;
    gw.dest_map_id = dest_map;
    gw.dest_x = dest_x;
    gw.dest_y = dest_y;
    gws.push_back(gw);
    console::ok("[CROSSMAP] Discovered portal (%d,%d) on map %u -> map %u (%d,%d)",
        src_x, src_y, src_map, dest_map, dest_x, dest_y);
    save_gateway_db();
}

inline const char* map_name(uint32_t id) {
    auto it = g_map_names.find(id);
    return it != g_map_names.end() ? it->second.c_str() : "???";
}

// ═══════════════════════════════════════════════════════════════════════
// ROUTE FINDER (BFS - finds shortest path between maps)
// ═══════════════════════════════════════════════════════════════════════

struct RouteStep {
    Gateway gateway;      // the gateway to use
    uint32_t from_map{0}; // source map
    uint32_t to_map{0};   // destination map
};

inline std::vector<RouteStep> find_route(uint32_t from_map, uint32_t to_map,
                                          uint64_t silver = 999999, uint32_t level = 999) {
    if (from_map == to_map) return {};
    if (!g_db_loaded) return {};

    struct Node {
        uint32_t map_id;
        int cost;
        int parent_idx; // index into visited array
        Gateway gateway; // gateway used to reach this node
    };

    std::vector<Node> visited;
    std::unordered_set<uint32_t> closed;
    std::queue<int> open; // indices into visited

    // Seed with start map
    visited.push_back({from_map, 0, -1, {}});
    open.push(0);
    closed.insert(from_map);

    while (!open.empty()) {
        int cur_idx = open.front();
        open.pop();
        auto& cur = visited[cur_idx];

        auto it = g_gateways.find(cur.map_id);
        if (it == g_gateways.end()) continue;

        for (auto& gw : it->second) {
            if (closed.count(gw.dest_map_id)) continue;

            // Check requirements
            if (gw.type == GatewayType::Npc) {
                if (gw.require_money > silver) continue;
                if (gw.require_level_above && level <= gw.require_level_above) continue;
                if (gw.require_level_below && level >= gw.require_level_below) continue;
            }

            int new_idx = (int)visited.size();
            visited.push_back({gw.dest_map_id, cur.cost + 1, cur_idx, gw});
            closed.insert(gw.dest_map_id);

            if (gw.dest_map_id == to_map) {
                // Reconstruct path
                std::vector<RouteStep> path;
                int idx = new_idx;
                while (idx > 0) {
                    auto& n = visited[idx];
                    path.push_back({n.gateway, visited[n.parent_idx].map_id, n.map_id});
                    idx = n.parent_idx;
                }
                std::reverse(path.begin(), path.end());
                return path;
            }
            open.push(new_idx);
        }
    }
    return {}; // no route found
}

// ═══════════════════════════════════════════════════════════════════════
// CROSS-MAP TASK EXECUTOR
// ═══════════════════════════════════════════════════════════════════════

enum class TaskState {
    Idle,
    WaitVipTeleport, // waiting for VIP teleport map change
    PathToGateway,   // pathfinding to portal/NPC position
    WaitArrival,     // waiting to arrive at gateway position
    NpcInteract,     // opening NPC dialog
    NpcDialog,       // clicking dialog options
    WaitMapChange,   // waiting for map to change
    Complete,
    Failed
};

inline TaskState g_task_state{TaskState::Idle};
inline int64_t g_vip_teleport_time{0};
inline std::vector<RouteStep> g_route;
inline int g_route_idx{0};
inline int g_npc_option_idx{0};
inline int64_t g_task_timer{0};
inline int g_task_retries{0};
inline uint32_t g_dest_map{0};
inline int g_dest_x{0}, g_dest_y{0};

inline bool is_traveling() {
    return g_task_state != TaskState::Idle && g_task_state != TaskState::Complete && g_task_state != TaskState::Failed;
}

inline void start_travel(uint32_t dest_map, int dest_x = 0, int dest_y = 0) {
    uint32_t cur_map = game::get_map_id();
    if (cur_map == dest_map) {
        console::info("[CROSSMAP] Already on map %u", dest_map);
        g_task_state = TaskState::Complete;
        return;
    }

    // Get hero level and silver for requirement checks
    uint32_t level = 130; // TODO: read actual level
    uint64_t silver = 0;
    {
        uintptr_t h = game::get_hero();
        if (h) game::safe_read(h + game::OFF_SILVER, silver);
    }

    auto route = find_route(cur_map, dest_map, silver, level);
    if (route.empty()) {
        console::error("[CROSSMAP] No route from %s(%u) to %s(%u)",
            map_name(cur_map), cur_map, map_name(dest_map), dest_map);
        g_task_state = TaskState::Failed;
        return;
    }

    console::ok("[CROSSMAP] Route found (%d steps):", (int)route.size());
    for (auto& s : route) {
        const char* type = s.gateway.type == GatewayType::Portal ? "Portal" : "NPC";
        console::info("  %s(%u) -> %s(%u) via %s at (%d,%d)",
            map_name(s.from_map), s.from_map,
            map_name(s.to_map), s.to_map,
            type, s.gateway.x, s.gateway.y);
    }

    // VIP optimization: find the last main city in the route and VIP teleport to it
    if (game::is_vip()) {
        auto get_vip_city = [](uint32_t map_id) -> int {
            if (map_id == 1002) return game::VIP_CITY_TC;
            if (map_id == 1011) return game::VIP_CITY_PC;
            if (map_id == 1020) return game::VIP_CITY_AC;
            if (map_id == 1000) return game::VIP_CITY_DC;
            if (map_id == 1015) return game::VIP_CITY_BI;
            return 0;
        };
        int last_city_step = -1;
        int last_city_vip = 0;
        for (int i = (int)route.size() - 1; i >= 0; i--) {
            int vip = get_vip_city(route[i].from_map);
            if (vip) {
                last_city_step = i;
                last_city_vip = vip;
                break;
            }
        }
        if (last_city_step > 0) { // step 0 means we're already there, no benefit
            int64_t now_ms = static_cast<int64_t>(GetTickCount64());
            int64_t cd = game::g_last_vip_teleport_time + game::VIP_TELEPORT_COOLDOWN_MS - now_ms;
            if (cd <= 0) {
                static const char* names[] = {"?","TC","PC","AC","DC","BI"};
                console::info("[CROSSMAP] VIP shortcut: teleport to %s, skip %d steps",
                    names[last_city_vip], last_city_step);
                game::queue_vip_teleport(last_city_vip);
                pathfinder::invalidate_grid();
                pathfinder::stop_pathfind();
                // Trim route: skip to the step that starts from the VIP city
                route.erase(route.begin(), route.begin() + last_city_step);
                // Set up route and enter wait state for teleport
                g_route = std::move(route);
                g_route_idx = 0;
                g_dest_map = dest_map;
                g_dest_x = dest_x;
                g_dest_y = dest_y;
                g_task_state = TaskState::WaitVipTeleport;
                g_task_retries = 0;
                g_vip_teleport_time = static_cast<int64_t>(GetTickCount64());
                return;
            }
        }
    }

    g_route = std::move(route);
    g_route_idx = 0;
    g_dest_map = dest_map;
    g_dest_x = dest_x;
    g_dest_y = dest_y;
    g_task_state = TaskState::PathToGateway;
    g_task_retries = 0;
}

inline void stop_travel() {
    g_task_state = TaskState::Idle;
    g_route.clear();
    g_route_idx = 0;
    pathfinder::stop_pathfind();
}

inline void tick(uintptr_t hero) {
    if (g_task_state == TaskState::Idle || g_task_state == TaskState::Complete || g_task_state == TaskState::Failed)
        return;

    int64_t now = static_cast<int64_t>(GetTickCount64());
    if (now < g_task_timer) return; // rate limit

    uint32_t cur_map = game::get_map_id();

    // Handle VIP teleport wait
    if (g_task_state == TaskState::WaitVipTeleport) {
        if (now - g_vip_teleport_time < 5000) return; // wait 5 sec for map load
        console::info("[CROSSMAP] Post-VIP teleport, now on map %u, continuing route", cur_map);
        pathfinder::invalidate_grid();
        g_task_state = TaskState::PathToGateway;
        return;
    }

    // Check if we've arrived at final destination
    if (cur_map == g_dest_map && g_route_idx >= (int)g_route.size()) {
        console::ok("[CROSSMAP] Arrived at destination %s(%u)!", map_name(g_dest_map), g_dest_map);
        g_task_state = TaskState::Complete;
        pathfinder::stop_pathfind();
        return;
    }

    if (g_route_idx >= (int)g_route.size()) {
        g_task_state = TaskState::Failed;
        return;
    }

    auto& step = g_route[g_route_idx];

    // If we're already on the destination map of this step, advance
    if (cur_map == step.to_map) {
        g_route_idx++;
        g_task_state = TaskState::PathToGateway;
        g_task_timer = now + 2000; // wait 2s after map change
        g_task_retries = 0;
        console::info("[CROSSMAP] Map changed to %s(%u), step %d/%d",
            map_name(cur_map), cur_map, g_route_idx, (int)g_route.size());
        return;
    }

    // If we're NOT on the expected source map, something went wrong
    if (cur_map != step.from_map) {
        console::warn("[CROSSMAP] Expected map %u but on %u, recalculating...",
            step.from_map, cur_map);
        start_travel(g_dest_map, g_dest_x, g_dest_y);
        return;
    }

    int hx = 0, hy = 0;
    game::get_pos(hx, hy);
    int gw_dist = std::max(std::abs(hx - step.gateway.x), std::abs(hy - step.gateway.y));

    switch (g_task_state) {
    case TaskState::PathToGateway: {
        console::info("[CROSSMAP] Heading to %s at (%d,%d) dist=%d",
            step.gateway.type == GatewayType::Portal ? "portal" : step.gateway.npc_name.c_str(),
            step.gateway.x, step.gateway.y, gw_dist);

        // Already close enough? Skip pathfinding
        if (step.gateway.type == GatewayType::Portal && gw_dist <= 2) {
            console::info("[CROSSMAP] Already at portal, waiting for map change...");
            g_task_state = TaskState::WaitMapChange;
            g_task_timer = now + 1000;
        } else if (step.gateway.type == GatewayType::Npc && gw_dist <= 5) {
            console::info("[CROSSMAP] Already near NPC, interacting...");
            g_task_state = TaskState::NpcInteract;
            g_npc_option_idx = 0;
            g_task_timer = now + 500;
        } else {
            pathfinder::start_pathfind(step.gateway.x, step.gateway.y);
            g_task_state = TaskState::WaitArrival;
            g_task_timer = now + 500;
        }
        break;
    }

    case TaskState::WaitArrival: {
        // Check if we're close enough
        if (step.gateway.type == GatewayType::Portal) {
            // For portals, just need to be on/near the tile — game auto-triggers
            if (gw_dist <= 2) {
                console::info("[CROSSMAP] At portal, waiting for map change...");
                g_task_state = TaskState::WaitMapChange;
                g_task_timer = now + 3000;
                pathfinder::stop_pathfind();
            } else if (!pathfinder::is_pathfinding()) {
                // Pathfinder stopped but not close enough — retry
                g_task_retries++;
                if (g_task_retries > 5) {
                    console::error("[CROSSMAP] Failed to reach portal after %d retries", g_task_retries);
                    g_task_state = TaskState::Failed;
                } else {
                    g_task_state = TaskState::PathToGateway;
                    g_task_timer = now + 1000;
                }
            }
        } else {
            // For NPCs, need to be within interaction range (~3 cells)
            if (gw_dist <= 5) {
                pathfinder::stop_pathfind();
                console::info("[CROSSMAP] Near NPC %s, interacting...", step.gateway.npc_name.c_str());
                g_task_state = TaskState::NpcInteract;
                g_npc_option_idx = 0;
                g_task_timer = now + 500;
            } else if (!pathfinder::is_pathfinding()) {
                g_task_retries++;
                if (g_task_retries > 5) {
                    g_task_state = TaskState::Failed;
                } else {
                    g_task_state = TaskState::PathToGateway;
                    g_task_timer = now + 1000;
                }
            }
        }
        g_task_timer = now + 500;
        break;
    }

    case TaskState::NpcInteract: {
        // Find the NPC entity nearby using the safe entity scanner
        uint32_t npc_id = 0;
        {
            auto nearby = entities::scan_nearby();
            for (auto& e : nearby) {
                if (step.gateway.npc_name == e.name) {
                    npc_id = e.id;
                    break;
                }
            }
        }

        if (npc_id) {
            game::queue_activate_npc(npc_id);
            console::info("[CROSSMAP] Activating NPC %s (id=%u)", step.gateway.npc_name.c_str(), npc_id);

            g_task_state = TaskState::NpcDialog;
            g_task_timer = now + 3000; // give time for dialog to open
        } else {
            console::warn("[CROSSMAP] NPC '%s' not found nearby", step.gateway.npc_name.c_str());
            g_task_retries++;
            if (g_task_retries > 10) {
                g_task_state = TaskState::Failed;
            } else {
                g_task_timer = now + 2000;
            }
        }
        break;
    }

    case TaskState::NpcDialog: {
        // Check if dialog is open and click options in sequence
        if (!game::is_dialog_open()) {
            // Dialog not open yet, retry interaction
            g_task_retries++;
            if (g_task_retries > 10) {
                g_task_state = TaskState::Failed;
            } else {
                g_task_state = TaskState::NpcInteract;
                g_task_timer = now + 2000;
            }
            break;
        }

        if (g_npc_option_idx >= (int)step.gateway.options.size()) {
            // All options clicked, wait for map change
            g_task_state = TaskState::WaitMapChange;
            g_task_timer = now + 3000;
            break;
        }

        // Read dialog options and click the right one
        auto opts = game::get_dialog_options();
        uint32_t target_opt = step.gateway.options[g_npc_option_idx];

        // The options array in Gateways.json uses indices (0-based)
        // Find the button option at that index
        int button_idx = 0;
        for (auto& o : opts) {
            if (o.type == 1) { // Button type
                if (button_idx == (int)target_opt) {
                    console::info("[CROSSMAP] Clicking dialog option %d (id=%d, text='%s')",
                        target_opt, o.id, o.text.c_str());
                    game::queue_answer_dialog(o.id);
                    g_npc_option_idx++;
                    g_task_timer = now + 1500;
                    break;
                }
                button_idx++;
            }
        }
        break;
    }

    case TaskState::WaitMapChange: {
        // Check if map changed
        if (cur_map != step.from_map) {
            // Map changed!
            g_route_idx++;
            g_task_state = TaskState::PathToGateway;
            g_task_timer = now + 3000; // wait for new map to load
            g_task_retries = 0;
        } else {
            g_task_retries++;
            if (g_task_retries > 15) { // 15 * 1s = 15s timeout
                console::error("[CROSSMAP] Map change timeout");
                g_task_state = TaskState::Failed;
            }
            g_task_timer = now + 1000;
        }
        break;
    }

    default: break;
    }
}

inline const char* state_str() {
    switch (g_task_state) {
    case TaskState::Idle: return "Idle";
    case TaskState::PathToGateway: return "Walking to gateway";
    case TaskState::WaitArrival: return "Approaching gateway";
    case TaskState::NpcInteract: return "Interacting with NPC";
    case TaskState::NpcDialog: return "In NPC dialog";
    case TaskState::WaitMapChange: return "Waiting for map change";
    case TaskState::Complete: return "Complete";
    case TaskState::Failed: return "Failed";
    default: return "Unknown";
    }
}

// Get all reachable maps from the gateway database
inline std::vector<std::pair<uint32_t, std::string>> get_all_maps() {
    std::vector<std::pair<uint32_t, std::string>> result;
    std::set<uint32_t> seen;
    for (auto& kv : g_gateways) {
        if (!seen.count(kv.first)) {
            seen.insert(kv.first);
            result.push_back({kv.first, map_name(kv.first)});
        }
        for (auto& g : kv.second) {
            if (!seen.count(g.dest_map_id)) {
                seen.insert(g.dest_map_id);
                result.push_back({g.dest_map_id, map_name(g.dest_map_id)});
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace crossmap
