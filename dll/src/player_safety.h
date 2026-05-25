#pragma once
#include "game.h"
#include "entity_scanner.h"
#include "pathfinder.h"
#include "cross_map.h"
#include "console.h"
#include <unordered_map>
#include <cstring>
#include <vector>
#include <string>
#include <mutex>

namespace player_safety {

// ── Player encounter response ───────────────────────────────────────
enum class PlayerAction : int {
    FleeOnly      = 0,  // jump away, resume when clear
    SafeSpot      = 1,  // travel to safe spot, wait, return, resume
    Disconnect    = 2,  // disconnect, wait, reconnect, resume
};

// ── GM/PM immediate response ────────────────────────────────────────
enum class GmAction : int {
    Disconnect    = 0,  // immediate disconnect + wait + reconnect
    SafeSpot      = 1,  // travel to safe spot + wait + return
};

// ── Config ──────────────────────────────────────────────────────────
struct Config {
    // Player avoidance
    bool enabled{false};
    int flee_distance{12};
    int max_encounters{3};
    int encounter_window_sec{120};
    PlayerAction player_action{PlayerAction::FleeOnly};
    // Safe spot (used by SafeSpot action for both player + GM)
    int safe_x{0}, safe_y{0};
    uint32_t safe_map{0};
    // Wait times for escalation / safe spot
    int wait_min_sec{60};
    int wait_max_sec{300};

    // GM/PM detection
    bool gm_detect{true};
    GmAction gm_action{GmAction::Disconnect};
    int gm_wait_min_sec{300};
    int gm_wait_max_sec{900};
    bool gm_play_sound{true};  // signal GUI to play alert sound
};

// ── Per-player encounter tracking ───────────────────────────────────
struct PlayerEncounter {
    int count{0};
    int64_t first_seen{0};
    int64_t last_seen{0};
    bool fled{false};
    char name[64]{};
};

// ── State machine ───────────────────────────────────────────────────
enum class State {
    Idle,
    Fleeing,              // jumping away from player
    TravelToSafe,         // crossmap traveling to safe spot
    WaitingAtSafe,        // sitting at safe spot
    TravelBack,           // crossmap traveling back to hunt spot
    WaitingReconnect,     // disconnected, timer running
    Returning,            // local pathfinding back to hunt pos
};

struct SafetyState {
    Config config;
    State state{State::Idle};

    // Encounter tracking
    std::unordered_map<uint32_t, PlayerEncounter> encounters;
    int total_encounter_count{0};
    int64_t window_start{0};

    // Flee
    uint32_t flee_from_id{0};

    // Wait timer
    int64_t wait_until{0};

    // Origin (hunt spot to return to)
    int origin_x{0}, origin_y{0};
    uint32_t origin_map{0};

    // Signals to dllmain (polled, not called from game thread)
    bool request_disconnect{false};
    bool request_reconnect{false};
    bool request_sound{false};       // signal GUI to play alert
    bool request_auto_start{false};  // signal to auto-start bot after reconnect

    // GUI log queue
    std::vector<std::string> log_queue;
    std::mutex log_mutex;
};

inline SafetyState g_state;

// ── Helpers ─────────────────────────────────────────────────────────

inline int rand_range(int lo, int hi) {
    if (lo >= hi) return lo;
    uint32_t seed = static_cast<uint32_t>(GetTickCount64()) * 1664525u + 1013904223u;
    return lo + (int)(seed % (uint32_t)(hi - lo + 1));
}

inline const char* timestamp() {
    static thread_local char buf[32];
    int64_t ms = static_cast<int64_t>(GetTickCount64());
    int sec = (int)((ms / 1000) % 86400);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
    return buf;
}

inline void log_gui(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(g_state.log_mutex);
    if (g_state.log_queue.size() < 50)
        g_state.log_queue.emplace_back(buf);
}

inline std::vector<std::string> drain_logs() {
    std::lock_guard<std::mutex> lock(g_state.log_mutex);
    std::vector<std::string> out;
    out.swap(g_state.log_queue);
    return out;
}

inline bool is_gm_or_pm(const entities::EntityInfo& e) {
    if (e.id >= 1000000 && e.id < 1000020) return true;
    if (strstr(e.name, "[PM]") || strstr(e.name, "[GM]") || strstr(e.name, "[Admin]"))
        return true;
    return false;
}

// ── Public API ──────────────────────────────────────────────────────

inline void configure(const Config& cfg) { g_state.config = cfg; }

inline void reset() {
    g_state.state = State::Idle;
    g_state.encounters.clear();
    g_state.total_encounter_count = 0;
    g_state.window_start = 0;
    g_state.flee_from_id = 0;
    g_state.request_disconnect = false;
    g_state.request_reconnect = false;
    g_state.request_sound = false;
    g_state.request_auto_start = false;
}

inline State get_state() { return g_state.state; }
inline bool is_busy() { return g_state.state != State::Idle; }

inline bool wants_disconnect() {
    bool d = g_state.request_disconnect;
    g_state.request_disconnect = false;
    return d;
}
inline bool wants_reconnect() {
    bool r = g_state.request_reconnect;
    g_state.request_reconnect = false;
    return r;
}
inline bool wants_sound() {
    bool s = g_state.request_sound;
    g_state.request_sound = false;
    return s;
}
inline bool wants_auto_start() {
    bool a = g_state.request_auto_start;
    g_state.request_auto_start = false;
    return a;
}

inline void set_origin(int x, int y, uint32_t map) {
    g_state.origin_x = x;
    g_state.origin_y = y;
    g_state.origin_map = map;
}

// ── Action executors ────────────────────────────────────────────────

inline void do_disconnect(int64_t now, int wait_min, int wait_max, const char* reason) {
    int wait_sec = rand_range(wait_min, wait_max);
    console::warn("[SAFETY] %s -- disconnecting for %d sec", reason, wait_sec);
    log_gui("[%s] [SAFETY] %s -- disconnecting for %d sec", timestamp(), reason, wait_sec);
    g_state.wait_until = now + wait_sec * 1000LL;
    g_state.state = State::WaitingReconnect;
    g_state.request_disconnect = true;
    g_state.request_auto_start = true;
    pathfinder::stop_pathfind();
    crossmap::stop_travel();
}

inline void do_safe_spot(int64_t now, int wait_min, int wait_max, const char* reason) {
    auto& cfg = g_state.config;
    int wait_sec = rand_range(wait_min, wait_max);
    console::warn("[SAFETY] %s -- going to safe spot, waiting %d sec", reason, wait_sec);
    log_gui("[%s] [SAFETY] %s -- safe spot (%d,%d map %u), wait %d sec",
        timestamp(), reason, cfg.safe_x, cfg.safe_y, cfg.safe_map, wait_sec);
    g_state.wait_until = now + wait_sec * 1000LL;
    pathfinder::stop_pathfind();
    crossmap::stop_travel();

    uint32_t cur_map = game::get_map_id();
    if (cfg.safe_map != 0 && cfg.safe_map != cur_map) {
        crossmap::start_travel(cfg.safe_map, cfg.safe_x, cfg.safe_y);
        g_state.state = State::TravelToSafe;
    } else {
        // Same map -- just jump away
        game::queue_jump(cfg.safe_x, cfg.safe_y);
        g_state.state = State::WaitingAtSafe;
    }
}

inline void do_flee(int hx, int hy, int px, int py) {
    auto& cfg = g_state.config;
    int flee_dist = (cfg.flee_distance < 12) ? cfg.flee_distance : 12;
    double base_angle = std::atan2((double)(hy - py), (double)(hx - px));

    // Try the ideal direction first, then fan out in 30-degree increments
    // looking for a walkable jump destination
    int best_x = hx, best_y = hy;
    bool found = false;

    for (int attempt = 0; attempt < 12 && !found; attempt++) {
        // Alternate: 0, +30, -30, +60, -60, +90, -90, ...
        int offset_deg = (attempt / 2 + 1) * 30 * ((attempt % 2 == 0) ? 1 : -1);
        if (attempt == 0) offset_deg = 0;
        double angle = base_angle + offset_deg * 3.14159 / 180.0;

        // Try from max distance down to half, find farthest valid jump
        for (int d = flee_dist; d >= flee_dist / 2; d -= 2) {
            int fx = hx + (int)(std::cos(angle) * d);
            int fy = hy + (int)(std::sin(angle) * d);
            if (pathfinder::can_jump_to(hx, hy, fx, fy)) {
                best_x = fx;
                best_y = fy;
                found = true;
                break;
            }
        }
    }

    pathfinder::stop_pathfind();
    if (found) {
        game::queue_jump(best_x, best_y);
        console::info("[SAFETY] Fleeing toward (%d,%d)", best_x, best_y);
        log_gui("[%s] [SAFETY] Fleeing toward (%d,%d)", timestamp(), best_x, best_y);
    } else {
        // Fallback: jump directly opposite even if not validated
        int fx = hx + (int)(std::cos(base_angle) * (flee_dist / 2));
        int fy = hy + (int)(std::sin(base_angle) * (flee_dist / 2));
        game::queue_jump(fx, fy);
        console::warn("[SAFETY] Fleeing (no clear path) toward (%d,%d)", fx, fy);
        log_gui("[%s] [SAFETY] Fleeing (no clear path) toward (%d,%d)", timestamp(), fx, fy);
    }
}

// ── Main tick ───────────────────────────────────────────────────────

inline void tick(uintptr_t hero, const std::vector<entities::EntityInfo>& nearby) {
    if (!hero) return;
    auto& s = g_state;
    auto& cfg = s.config;
    if (!cfg.enabled) return;

    int64_t now = static_cast<int64_t>(GetTickCount64());
    int hx = 0, hy = 0;
    game::get_pos(hx, hy);

    // ── Handle non-idle states ──────────────────────────────────────

    if (s.state == State::WaitingReconnect) {
        if (now >= s.wait_until) {
            console::ok("[SAFETY] Reconnect timer expired");
            s.request_reconnect = true;
            s.request_auto_start = true;
            reset();
        }
        return;
    }

    if (s.state == State::TravelToSafe) {
        uint32_t cur_map = game::get_map_id();
        if (cur_map == cfg.safe_map) {
            int d = (std::max)(std::abs(hx - cfg.safe_x), std::abs(hy - cfg.safe_y));
            if (d <= 5) {
                crossmap::stop_travel();
                console::ok("[SAFETY] Arrived at safe spot");
                log_gui("[%s] [SAFETY] Arrived at safe spot", timestamp());
                s.state = State::WaitingAtSafe;
                return;
            }
        }
        // Crossmap handles travel; just wait
        if (!crossmap::is_traveling()) {
            crossmap::start_travel(cfg.safe_map, cfg.safe_x, cfg.safe_y);
        }
        return;
    }

    if (s.state == State::WaitingAtSafe) {
        if (now >= s.wait_until) {
            console::ok("[SAFETY] Wait over, returning to hunt spot");
            log_gui("[%s] [SAFETY] Wait over, returning", timestamp());
            s.state = State::TravelBack;
        }
        return;
    }

    if (s.state == State::TravelBack) {
        uint32_t cur_map = game::get_map_id();
        if (cur_map == s.origin_map) {
            int d = (std::max)(std::abs(hx - s.origin_x), std::abs(hy - s.origin_y));
            if (d <= 5) {
                crossmap::stop_travel();
                console::ok("[SAFETY] Back at hunting spot");
                log_gui("[%s] [SAFETY] Back at hunting spot", timestamp());
                s.state = State::Idle;
                return;
            }
            // On correct map, local pathfind
            crossmap::stop_travel();
            s.state = State::Returning;
            return;
        }
        if (!crossmap::is_traveling()) {
            crossmap::start_travel(s.origin_map, s.origin_x, s.origin_y);
        }
        return;
    }

    if (s.state == State::Returning) {
        uint32_t cur_map = game::get_map_id();
        if (cur_map == s.origin_map) {
            int d = (std::max)(std::abs(hx - s.origin_x), std::abs(hy - s.origin_y));
            if (d <= 5) {
                pathfinder::stop_pathfind();
                console::ok("[SAFETY] Back at hunting spot");
                log_gui("[%s] [SAFETY] Back at hunting spot", timestamp());
                s.state = State::Idle;
                return;
            }
        }
        if (!pathfinder::is_pathfinding())
            pathfinder::start_pathfind(s.origin_x, s.origin_y);
        return;
    }

    // ── Scan for players in entity list ─────────────────────────────

    struct Threat { uint32_t id; int x, y, dist; char name[64]; };

    Threat closest_player{0, 0, 0, 9999, {}};
    Threat closest_gm{0, 0, 0, 9999, {}};

    for (auto& e : nearby) {
        if (!e.is_player()) continue;
        if (e.is_dead) continue;
        int gd = (std::max)(std::abs(hx - e.x), std::abs(hy - e.y));

        bool gm = cfg.gm_detect && is_gm_or_pm(e);

        if (gm && gd < closest_gm.dist) {
            closest_gm.id = e.id;
            closest_gm.x = e.x; closest_gm.y = e.y;
            closest_gm.dist = gd;
            strncpy_s(closest_gm.name, e.name, _TRUNCATE);
        }
        if (!gm && gd < closest_player.dist) {
            closest_player.id = e.id;
            closest_player.x = e.x; closest_player.y = e.y;
            closest_player.dist = gd;
            strncpy_s(closest_player.name, e.name, _TRUNCATE);
        }
    }

    // ── GM/PM -- immediate action, no encounter counting ────────────
    if (closest_gm.id != 0) {
        console::error("[SAFETY] GM/PM '%s' detected! (id=%u at %d,%d)",
            closest_gm.name, closest_gm.id, closest_gm.x, closest_gm.y);
        log_gui("[%s] [SAFETY] *** GM/PM '%s' detected at (%d,%d) ***",
            timestamp(), closest_gm.name, closest_gm.x, closest_gm.y);

        s.origin_x = hx; s.origin_y = hy;
        s.origin_map = game::get_map_id();

        if (cfg.gm_play_sound)
            s.request_sound = true;

        switch (cfg.gm_action) {
        case GmAction::Disconnect:
            do_disconnect(now, cfg.gm_wait_min_sec, cfg.gm_wait_max_sec, "GM/PM detected");
            break;
        case GmAction::SafeSpot:
            do_safe_spot(now, cfg.gm_wait_min_sec, cfg.gm_wait_max_sec, "GM/PM detected");
            break;
        }
        s.encounters.clear();
        s.total_encounter_count = 0;
        s.window_start = 0;
        return;
    }

    // ── No player threat ────────────────────────────────────────────
    if (closest_player.id == 0) {
        if (s.state == State::Fleeing)
            s.state = State::Idle;
        return;
    }

    // ── Encounter window expiry ─────────────────────────────────────
    if (s.window_start > 0 && now - s.window_start > cfg.encounter_window_sec * 1000LL) {
        s.encounters.clear();
        s.total_encounter_count = 0;
        s.window_start = 0;
    }

    // ── Track encounter (log once per sighting) ─────────────────────
    auto& enc = s.encounters[closest_player.id];
    if (enc.count == 0) {
        enc.first_seen = now;
        enc.count = 1;
        enc.fled = false;
        strncpy_s(enc.name, closest_player.name, _TRUNCATE);
        s.total_encounter_count++;
        if (s.window_start == 0) s.window_start = now;

        console::warn("[SAFETY] Player '%s' detected at (%d,%d) -- encounter #%d",
            closest_player.name, closest_player.x, closest_player.y, s.total_encounter_count);
        log_gui("[%s] [SAFETY] Player '%s' at (%d,%d) -- encounter #%d",
            timestamp(), closest_player.name, closest_player.x, closest_player.y,
            s.total_encounter_count);

    } else if (enc.fled && now - enc.last_seen > 5000) {
        enc.count++;
        enc.fled = false;
        s.total_encounter_count++;

        console::warn("[SAFETY] Player '%s' reappeared at (%d,%d) -- #%d (total: %d)",
            closest_player.name, closest_player.x, closest_player.y, enc.count, s.total_encounter_count);
        log_gui("[%s] [SAFETY] Player '%s' reappeared at (%d,%d) -- #%d (total: %d)",
            timestamp(), closest_player.name, closest_player.x, closest_player.y,
            enc.count, s.total_encounter_count);
    }
    enc.last_seen = now;

    // ── Check escalation threshold ──────────────────────────────────
    if (s.total_encounter_count >= cfg.max_encounters) {
        s.origin_x = hx; s.origin_y = hy;
        s.origin_map = game::get_map_id();

        char reason[128];
        snprintf(reason, sizeof(reason), "Max encounters (%d) -- last: '%s'",
            cfg.max_encounters, closest_player.name);

        switch (cfg.player_action) {
        case PlayerAction::SafeSpot:
            do_safe_spot(now, cfg.wait_min_sec, cfg.wait_max_sec, reason);
            break;
        case PlayerAction::Disconnect:
            do_disconnect(now, cfg.wait_min_sec, cfg.wait_max_sec, reason);
            break;
        default: break; // FleeOnly -- just keep fleeing
        }

        s.encounters.clear();
        s.total_encounter_count = 0;
        s.window_start = 0;

        if (cfg.player_action != PlayerAction::FleeOnly)
            return;
    }

    // ── Flee from player (always, on every detection) ───────────────
    if (s.state != State::Fleeing || s.flee_from_id != closest_player.id) {
        s.flee_from_id = closest_player.id;
        s.state = State::Fleeing;
        enc.fled = true;
        do_flee(hx, hy, closest_player.x, closest_player.y);
    }
}

} // namespace player_safety
