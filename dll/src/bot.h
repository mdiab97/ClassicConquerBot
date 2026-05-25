#pragma once
#include "game.h"
#include "entity_scanner.h"
#include "pathfinder.h"
#include "console.h"
#include "player_safety.h"

// Forward declarations for auto_deposit (defined in auto_deposit.h, included after bot.h)
namespace auto_deposit {
    bool is_active();
    bool should_auto_deposit(int max_items);
    void start(int max_items);
}
#include <nlohmann/json.hpp>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <random>
#include <fstream>
#include <unordered_map>

namespace bot {

struct PotionInfo {
    uint32_t type_id{0};
    uint32_t heal_amount{0};
};

struct ItemTypeInfo {
    uint32_t type_id{0};
    uint16_t attack_range{0};  // attackRange from itemtype.json
};

struct MpPotionInfo {
    uint32_t type_id{0};
    uint32_t mana_amount{0};
};

struct MagicTypeInfo {
    uint32_t magic_type{0};
    int level{0};
    int action_sort{0};  // 1=single target, etc
    int distance{0};
    int mp_cost{0};
    int power{0};
    bool is_xp{false};
    std::string name;
};

struct BotConfig {
    bool auto_hp_pot{true};
    float hp_potion_pct{0.80f};
    std::vector<PotionInfo> potions;       // HP potions
    bool auto_mp_pot{false};
    float mp_potion_pct{0.50f};
    std::vector<MpPotionInfo> mp_potions;  // MP potions
    std::vector<uint32_t> pickup_types;
    uint32_t pickup_potion_threshold{5};
    bool pickup_hp_pots{true};             // pickup HP potions when low
    bool pickup_mp_pots{true};             // pickup MP potions when low
    bool loot_plus{false};               // loot any item with plus >= 1
    int loot_min_quality{0};             // 0=disabled, 5=Normal+, 6=Refined+, 7=Unique+, 8=Elite+, 9=Super
    // Monster filter: 0=disabled(hunt all), 1=blacklist(skip listed), 2=whitelist(only listed)
    int monster_filter_mode{0};
    std::vector<std::string> monster_filter_names;
    // Humanization
    int reaction_min_ms{200};
    int reaction_max_ms{600};
    int attack_cooldown_ms{1000};
    int attack_cooldown_xp_ms{100}; // faster attacks during cyclone
    int pickup_cooldown_ms{500};
    int action_range{5};
    int jump_scatter{2};
    int item_notice_min_ms{1000};
    int item_notice_max_ms{3000};
    // Auto-deposit
    bool auto_deposit{false};
    int deposit_threshold{39};           // trigger deposit when inventory >= this
    // XP Skill (Cyclone, Fly, Superman, etc)
    bool use_xp_skill{true};
    uint32_t xp_skill_id{1110};          // magic ID to cast when XP is full
    int xp_skill_delay_ms{500};          // delay between casts
    // Scatter skill
    bool use_scatter{false};
    uint32_t scatter_magic_id{8001};
    int scatter_cooldown_ms{1500};       // minimum ms between scatter casts
    double scatter_range{12.0};          // max hit distance from hero
    double scatter_half_angle{120.0};    // half-angle of the sector in degrees
    int scatter_min_targets{2};          // only cast if we predict this many hits
    bool scatter_predict_hits{true};     // blacklist predicted hits to keep moving forward
    // Magic hunt mode (hunt_mode=3)
    uint32_t magic_id{1000};             // magic type ID to cast
    int magic_cast_delay_ms{500};        // delay between casts
    int magic_safe_dist{3};              // jump away if any monster gets closer than this
    // Kiting (ranged combat)
    bool kite_enabled{false};
    int kite_min_dist{3};                // back away if monster gets closer than this
    // Melee AoE cluster targeting
    bool prefer_clusters{false};         // prefer targets with nearby monsters for AoE procs
    int cluster_radius{3};               // Chebyshev radius to count nearby monsters
    int cluster_min_stay{2};             // min monsters to stay committed to a cluster
    // Explore
    int explore_radius{80};              // radius around home to explore when no targets
    // Hunt mode: 0=melee, 1=ranged, 2=scatter, 3=magic
    int hunt_mode{0};
    // Player safety
    bool safety_enabled{false};
    int safety_flee_distance{12};
    int safety_max_encounters{3};
    int safety_encounter_window{120};
    int safety_player_action{0};         // 0=flee, 1=safe spot, 2=disconnect
    int safety_safe_x{0}, safety_safe_y{0};
    uint32_t safety_safe_map{0};
    int safety_wait_min{60};
    int safety_wait_max{300};
    // GM/PM detection
    bool safety_gm_detect{true};
    int safety_gm_action{0};             // 0=disconnect, 1=safe spot
    int safety_gm_wait_min{300};
    int safety_gm_wait_max{900};
    bool safety_gm_sound{true};
};

inline std::atomic<bool> g_running{false};
inline bool g_has_xp_skill{false};
inline bool g_has_scatter_magic{false};
inline int64_t g_last_scatter_time{0};
inline int64_t g_last_magic_cast_time{0};
inline BotConfig g_config;
inline std::unordered_map<uint32_t, ItemTypeInfo> g_itemtype_db;

// Magic type database: key = (magic_type * 100 + level)
inline std::unordered_map<uint32_t, MagicTypeInfo> g_magictype_db;

// Get the magic info for a specific type+level
inline const MagicTypeInfo* get_magic_info(uint32_t magic_type, int level) {
    auto it = g_magictype_db.find(magic_type * 100 + level);
    if (it != g_magictype_db.end()) return &it->second;
    return nullptr;
}

// Hero's known magics (populated on bot start from game::get_magic_list)
inline std::vector<game::MagicInfo> g_hero_magics;
inline uint32_t g_target_id{0};
inline int g_cluster_cx{0}, g_cluster_cy{0};  // current cluster center
inline int g_cluster_count{0};                 // monsters in cluster when we committed
inline std::unordered_map<uint32_t, int64_t> g_unreachable_targets;
inline int64_t g_last_attack_time{0};
inline int64_t g_last_pickup_time{0};
inline int64_t g_last_tick_time{0};
inline int64_t g_last_xp_skill_time{0};

// Pickup blacklist: item_id -> expiry tick (skip items that couldn't be picked up)
inline std::unordered_map<uint32_t, int64_t> g_pickup_blacklist;
// Track failed pickup attempts: item_id -> attempt count
inline std::unordered_map<uint32_t, int> g_pickup_attempts;
static constexpr int MAX_PICKUP_ATTEMPTS = 3; // blacklist after this many tries

// Item notice delay: item_id -> tick when item becomes "noticed"
// Items are invisible to the bot until their notice time passes
inline std::unordered_map<uint32_t, int64_t> g_item_notice_time;

// Jump rollback detection: blacklist ZONES that cause rollbacks
// When a jump from A to B gets rolled back, we blacklist a circular zone
// around B so the bot won't try jumping anywhere near there from near A.
struct JumpRecord {
    int from_x{0}, from_y{0};
    int to_x{0}, to_y{0};
    int64_t time{0};
};
struct BlockedZone {
    int center_x, center_y;  // center of the blocked area (failed destination)
    int radius;              // how wide the blocked zone is (grows with repeat fails)
    int64_t expiry;
};
inline JumpRecord g_last_jump;
inline std::vector<BlockedZone> g_blocked_zones;
inline int g_consecutive_rollbacks{0};        // escalates zone radius

// Meteor auto-pack
constexpr uint32_t METEOR_TYPE_ID = 1088001;
constexpr int METEOR_PACK_COUNT = 10;
inline int64_t g_last_meteor_check{0};

// Exploration state
inline int g_home_x{0}, g_home_y{0};
inline int g_explore_index{0};
inline std::vector<std::pair<int,int>> g_explore_line; // current line of waypoints to follow
inline int g_explore_line_idx{0};                      // progress along current line
inline std::vector<std::pair<int,int>> g_visited;
inline int64_t g_last_explore_time{0};
constexpr int VISION_RANGE = 32;
constexpr int EXPLORE_GRID_SIZE = 28;
constexpr int EXPLORE_COOLDOWN_MS = 500;

// RNG - no static locals (magic statics deadlock on game thread)
inline std::mt19937 g_rng{static_cast<unsigned>(12345)};
inline bool g_rng_seeded{false};

inline int rand_range(int lo, int hi) {
    if (lo >= hi) return lo;
    if (!g_rng_seeded) {
        g_rng.seed(static_cast<unsigned>(GetTickCount()));
        g_rng_seeded = true;
    }
    return lo + static_cast<int>(g_rng() % static_cast<unsigned>(hi - lo + 1));
}

// ═══════════════════════════════════════════════════════════════════════
// FAST SCANNING - no VirtualQuery, uses SEH directly
// ═══════════════════════════════════════════════════════════════════════

// Fast pointer read - no VirtualQuery, just direct access
// Since we're on the game thread reading game data, pointers should be valid
template<typename T>
inline T fast_read(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}

inline void fast_memcpy(void* dst, uintptr_t src, size_t n) {
    memcpy(dst, reinterpret_cast<void*>(src), n);
}

inline std::vector<entities::EntityInfo> fast_scan_nearby() {
    std::vector<entities::EntityInfo> result;
    uintptr_t base = game::get_base();
    uintptr_t ps = base + entities::OFF_PLAYERSET;

    uintptr_t buffer = fast_read<uintptr_t>(ps + 0x10);
    uint64_t capacity = fast_read<uint64_t>(ps + 0x18);
    uint64_t head = fast_read<uint64_t>(ps + 0x20);
    uint64_t count = fast_read<uint64_t>(ps + 0x28);
    if (!buffer || count == 0 || count > 500 || capacity == 0) return result;

    uint64_t mask = capacity - 1;
    for (uint64_t i = 0; i < count; i++) {
        uintptr_t node_ptr = fast_read<uintptr_t>(buffer + (mask & (head + i)) * 8);
        if (!node_ptr) continue;
        uintptr_t role = fast_read<uintptr_t>(node_ptr);
        if (!role) continue;

        entities::EntityInfo e;
        e.id = fast_read<uint32_t>(role + game::OFF_ID);
        if (e.id == 0) continue;
        e.x = fast_read<int>(role + game::OFF_POS_X);
        e.y = fast_read<int>(role + game::OFF_POS_Y);
        e.type = fast_read<int>(role + game::OFF_TYPE);
        e.role_kind = fast_read<int>(role + game::OFF_ROLE_KIND);
        e.npc_sort = fast_read<int>(role + game::OFF_NPC_SORT);
        e.look_type = fast_read<uint16_t>(role + game::OFF_LOOK_TYPE);
        int64_t sf = fast_read<int64_t>(role + game::OFF_STATUS_FLAG);
        e.is_dead = (sf & game::STATUS_DEAD) != 0 || (sf & game::STATUS_GHOST) != 0;
        fast_memcpy(e.name, role + game::OFF_NAME, 16);
        e.name[16] = 0;
        result.push_back(e);
    }
    return result;
}

inline std::vector<entities::GroundItem> fast_scan_ground_items() {
    std::vector<entities::GroundItem> result;
    uintptr_t base = game::get_base();
    // GameMap is at a fixed global address, NOT a pointer to dereference
    uintptr_t gm = base + entities::OFF_GAMEMAP;

    uintptr_t vec_begin = fast_read<uintptr_t>(gm + entities::OFF_ITEM_VEC_BEGIN);
    uintptr_t vec_end = fast_read<uintptr_t>(gm + entities::OFF_ITEM_VEC_END);
    if (!vec_begin || vec_end <= vec_begin) return result;

    size_t count = (vec_end - vec_begin) / 16;
    if (count > 500) count = 500;

    for (size_t i = 0; i < count; i++) {
        uintptr_t node = fast_read<uintptr_t>(vec_begin + i * 16);
        if (!node) continue;
        entities::GroundItem gi;
        gi.item_id = fast_read<uint32_t>(node + 0x00);
        gi.type_id = fast_read<uint32_t>(node + 0x04);
        gi.x = fast_read<int>(node + 0x08);
        gi.y = fast_read<int>(node + 0x0C);
        // Plus at C2DMapItem+0x48 (72), C2DMapItem ptr at node+0x10
        uintptr_t c2d = fast_read<uintptr_t>(node + 0x10);
        if (c2d) gi.plus = fast_read<uint8_t>(c2d + 0x48);
        if (gi.item_id > 0) result.push_back(gi);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════

// Get the hero's weapon attack range from itemtype DB
inline int get_weapon_attack_range() {
    uint32_t wep_type = game::get_weapon_type();
    if (wep_type == 0) return 1;
    auto it = g_itemtype_db.find(wep_type);
    if (it != g_itemtype_db.end() && it->second.attack_range > 0)
        return static_cast<int>(it->second.attack_range);
    return 1; // default melee range
}

inline double dist(int x1, int y1, int x2, int y2) {
    double dx = static_cast<double>(x2 - x1);
    double dy = static_cast<double>(y2 - y1);
    return std::sqrt(dx * dx + dy * dy);
}

// Game uses Chebyshev distance for attack range checks
inline int game_dist(int x1, int y1, int x2, int y2) {
    return (std::max)(std::abs(x2 - x1), std::abs(y2 - y1));
}

inline bool is_monster(const entities::EntityInfo& e) {
    if (e.is_dead) return false;
    if (!e.is_monster()) return false;
    // Monster name filter
    if (g_config.monster_filter_mode == 1) {
        // Blacklist: skip if name matches any entry
        for (auto& n : g_config.monster_filter_names)
            if (n == e.name) return false;
    } else if (g_config.monster_filter_mode == 2) {
        // Whitelist: only hunt if name matches an entry
        bool found = false;
        for (auto& n : g_config.monster_filter_names)
            if (n == e.name) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

inline bool is_hp_potion_type(uint32_t type_id) {
    for (auto& p : g_config.potions)
        if (p.type_id == type_id) return true;
    return false;
}

inline bool is_mp_potion_type(uint32_t type_id) {
    for (auto& p : g_config.mp_potions)
        if (p.type_id == type_id) return true;
    return false;
}

inline bool is_potion_type(uint32_t type_id) {
    return is_hp_potion_type(type_id) || is_mp_potion_type(type_id);
}

inline bool is_pickup_type(uint32_t type_id) {
    for (auto t : g_config.pickup_types)
        if (t == type_id) return true;
    return false;
}

inline int get_item_quality(uint32_t type_id) { return type_id % 10; }
// 9=Super, 8=Elite, 7=Unique, 6=Refined, 5=Normal

inline bool is_loot_eligible(const entities::GroundItem& gi, bool need_hp_pots, bool need_mp_pots) {
    // Explicit pickup type list
    if (is_pickup_type(gi.type_id)) return true;
    // HP Potions when low
    if (need_hp_pots && g_config.pickup_hp_pots && is_hp_potion_type(gi.type_id)) return true;
    // MP Potions when low
    if (need_mp_pots && g_config.pickup_mp_pots && is_mp_potion_type(gi.type_id)) return true;
    // Plus items
    if (g_config.loot_plus && gi.plus >= 1) return true;
    // Quality items
    if (g_config.loot_min_quality > 0 && get_item_quality(gi.type_id) >= g_config.loot_min_quality) return true;
    return false;
}

// Jump toward target, landing within action_range (not on top of it)
inline game::CMyPos direct_jump(int hx, int hy, int tx, int ty, int action_range = 0) {
    double d = dist(hx, hy, tx, ty);
    int dest_x = tx, dest_y = ty;

    if (action_range > 0 && d > action_range) {
        // Stop short: land action_range cells away from target (on the line from hero to target)
        double stop_d = d - action_range;
        double ratio = stop_d / d;
        dest_x = hx + static_cast<int>(std::round((tx - hx) * ratio));
        dest_y = hy + static_cast<int>(std::round((ty - hy) * ratio));
    }

    // Clamp to max jump distance
    double jd = dist(hx, hy, dest_x, dest_y);
    if (jd > pathfinder::MAX_JUMP_DIST) {
        double ratio = static_cast<double>(pathfinder::MAX_JUMP_DIST - 1) / jd;
        dest_x = hx + static_cast<int>(std::round((dest_x - hx) * ratio));
        dest_y = hy + static_cast<int>(std::round((dest_y - hy) * ratio));
    }
    if (dest_x == hx && dest_y == hy) dest_x += 1;
    return {dest_x, dest_y};
}

// Jump to a random position near target, guaranteed within action_range of target
inline game::CMyPos scatter_jump(int hx, int hy, int tx, int ty, int scatter, int max_action_range) {
    // Try random offsets, pick one within action_range of target AND jumpable from hero
    for (int attempt = 0; attempt < 10; attempt++) {
        int dx = rand_range(-scatter, scatter);
        int dy = rand_range(-scatter, scatter);
        int cx = tx + dx;
        int cy = ty + dy;
        if (dist(cx, cy, tx, ty) > max_action_range) continue;
        double jd = dist(hx, hy, cx, cy);
        if (jd > pathfinder::MAX_JUMP_DIST) continue;
        if (cx == hx && cy == hy) continue;
        return {cx, cy};
    }
    // Fallback: direct jump
    return direct_jump(hx, hy, tx, ty);
}

// Choose jump strategy: direct during cyclone for speed, scattered otherwise for humanization
inline game::CMyPos approach_jump(int hx, int hy, int tx, int ty, int scatter, int max_action_range) {
    if (game::is_cyclone())
        return direct_jump(hx, hy, tx, ty, max_action_range);
    return scatter_jump(hx, hy, tx, ty, scatter, max_action_range);
}

// Check if a destination falls inside any blocked zone
inline bool is_jump_blocked(int tx, int ty) {
    for (auto& z : g_blocked_zones) {
        if (dist(z.center_x, z.center_y, tx, ty) <= z.radius)
            return true;
    }
    return false;
}
// Two-arg overload used by approach_jump_safe and explore
inline bool is_jump_blacklisted(int tx, int ty) { return is_jump_blocked(tx, ty); }
// Four-arg overload for compatibility
inline bool is_jump_blacklisted(int /*fx*/, int /*fy*/, int tx, int ty) { return is_jump_blocked(tx, ty); }

// Issue a jump command and record it for rollback detection
// SetCommand — calls hero vtable+0x1D8 (for jump, pickup, attack commands)
inline void call_set_command(uintptr_t hero, void* cmd) {
    uintptr_t vtable = *reinterpret_cast<uintptr_t*>(hero);
    uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtable + 0x1D8);
    reinterpret_cast<void(__fastcall*)(uintptr_t, void*)>(fn)(hero, cmd);
}

// Returns false if blacklisted. Clamps to max 12 cells.
static constexpr int MAX_JUMP = 12;

// Oscillation detection: ring buffer of recent jump destinations
struct JumpHistoryEntry { int x{0}, y{0}; int64_t time{0}; };
inline JumpHistoryEntry g_jump_history[8]{};
inline int g_jump_history_idx{0};

inline bool is_oscillating(int tx, int ty) {
    int64_t now = (int64_t)GetTickCount64();
    int revisits = 0;
    for (int i = 0; i < 8; i++) {
        auto& r = g_jump_history[i];
        if (r.time == 0) continue;
        if (now - r.time > 5000) continue;
        if (game_dist(r.x, r.y, tx, ty) <= 3) revisits++;
    }
    return revisits >= 2;
}

inline void record_jump(int tx, int ty) {
    g_jump_history[g_jump_history_idx].x = tx;
    g_jump_history[g_jump_history_idx].y = ty;
    g_jump_history[g_jump_history_idx].time = (int64_t)GetTickCount64();
    g_jump_history_idx = (g_jump_history_idx + 1) % 8;
}

inline bool do_jump(uintptr_t hero, int hx, int hy, int tx, int ty) {
    // Hard clamp to MAX_JUMP distance
    double d = dist(hx, hy, tx, ty);
    if (d > MAX_JUMP) {
        double ratio = static_cast<double>(MAX_JUMP - 1) / d;
        tx = hx + static_cast<int>(std::round((tx - hx) * ratio));
        ty = hy + static_cast<int>(std::round((ty - hy) * ratio));
    }
    if (tx == hx && ty == hy) return false;
    if (is_jump_blocked(tx, ty)) return false;
    if (!pathfinder::can_jump_to(hx, hy, tx, ty)) return false;

    // Oscillation detection: block jumps to recently visited areas
    if (is_oscillating(tx, ty)) {
        console::warn("[JUMP] Oscillation detected at (%d,%d), blocking", tx, ty);
        return false;
    }

    uint8_t jcmd[0x108]{};
    *reinterpret_cast<int32_t*>(jcmd + 0x00) = game::CMD_JUMP;
    *reinterpret_cast<int32_t*>(jcmd + 0x0C) = tx;
    *reinterpret_cast<int32_t*>(jcmd + 0x10) = ty;
    call_set_command(hero, jcmd);
    record_jump(tx, ty);
    g_last_jump = {hx, hy, tx, ty, static_cast<int64_t>(GetTickCount64())};
    return true;
}

// Get an approach jump position, avoiding blacklisted spots
inline game::CMyPos approach_jump_safe(int hx, int hy, int tx, int ty, int scatter, int max_action_range) {
    // Try up to 8 different positions, skipping blacklisted ones
    for (int attempt = 0; attempt < 8; attempt++) {
        game::CMyPos pos;
        if (game::is_cyclone() && attempt == 0) {
            pos = direct_jump(hx, hy, tx, ty, max_action_range);
        } else {
            // Force scatter to find alternative positions
            int s = (scatter > 0) ? scatter : 2;
            pos = scatter_jump(hx, hy, tx, ty, s + attempt, max_action_range + attempt);
        }
        if (!is_jump_blocked(pos.x, pos.y) && pathfinder::can_jump_to(hx, hy, pos.x, pos.y)) return pos;
    }
    // All blocked - return best attempt anyway, do_jump will reject if still blocked
    return direct_jump(hx, hy, tx, ty, max_action_range);
}

// ═══════════════════════════════════════════════════════════════════════
// EXPLORATION - Line-based movement within a circular area
//
// Picks a random walkable destination within explore_radius, traces a
// line of cells to it (rejecting if 3+ consecutive blocked cells),
// then follows the line by jumping to the farthest reachable point
// along it each tick. When the line is done, picks a new destination.
// ═══════════════════════════════════════════════════════════════════════

inline bool was_visited_recently(int x, int y) {
    int gx = x / EXPLORE_GRID_SIZE;
    int gy = y / EXPLORE_GRID_SIZE;
    for (auto& [vx, vy] : g_visited)
        if (vx == gx && vy == gy) return true;
    return false;
}

inline void mark_visited(int x, int y) {
    int gx = x / EXPLORE_GRID_SIZE;
    int gy = y / EXPLORE_GRID_SIZE;
    for (auto& [vx, vy] : g_visited)
        if (vx == gx && vy == gy) return; // already there
    g_visited.push_back({gx, gy});
    if (g_visited.size() > 100)
        g_visited.erase(g_visited.begin(), g_visited.begin() + 30);
}

// Bresenham line from (x0,y0) to (x1,y1) - returns all cells along the line
inline std::vector<std::pair<int,int>> trace_line(int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int,int>> result;
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int cx = x0, cy = y0;
    for (int i = 0; i < 500; i++) { // safety limit
        result.push_back({cx, cy});
        if (cx == x1 && cy == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; cx += sx; }
        if (e2 < dx)  { err += dx; cy += sy; }
    }
    return result;
}

// Try to create a walkable line from (fx,fy) to (tx,ty).
// Returns the line if valid (fewer than 3 consecutive blocked cells), empty if not.
inline std::vector<std::pair<int,int>> try_create_line(int fx, int fy, int tx, int ty) {
    auto line = trace_line(fx, fy, tx, ty);
    int blocked = 0;
    for (auto& [cx, cy] : line) {
        if (pathfinder::is_blocked(cx, cy))
            blocked++;
        else
            blocked = 0;
        if (blocked >= 3) return {}; // path too blocked
    }
    return line;
}

// Pick a new destination — bias toward visible monsters or unvisited areas
inline bool pick_new_line(int hx, int hy) {
    int radius = g_config.explore_radius;

    // First: try to move toward the farthest visible monster cluster
    // This prevents wandering to empty areas when monsters are visible but spread out
    auto nearby = fast_scan_nearby();
    std::vector<std::pair<int,int>> monster_positions;
    for (auto& e : nearby) {
        if (!is_monster(e)) continue;
        monster_positions.push_back({e.x, e.y});
    }

    if (!monster_positions.empty()) {
        // Find monster centroid and try to move toward it
        double cx = 0, cy = 0;
        for (auto& [mx, my] : monster_positions) { cx += mx; cy += my; }
        cx /= monster_positions.size(); cy /= monster_positions.size();
        int tx = (int)std::round(cx), ty = (int)std::round(cy);

        if (dist(hx, hy, tx, ty) > 5) {
            auto line = try_create_line(hx, hy, tx, ty);
            if (!line.empty() && line.size() > 3) {
                g_explore_line = line;
                g_explore_line_idx = 0;
                return true;
            }
        }

        // Centroid blocked — try individual monster positions (farthest first for coverage)
        std::sort(monster_positions.begin(), monster_positions.end(),
            [hx, hy](auto& a, auto& b) { return dist(hx, hy, a.first, a.second) > dist(hx, hy, b.first, b.second); });
        for (auto& [mx, my] : monster_positions) {
            if (dist(hx, hy, mx, my) < 8) continue;
            auto line = try_create_line(hx, hy, mx, my);
            if (!line.empty() && line.size() > 3) {
                g_explore_line = line;
                g_explore_line_idx = 0;
                return true;
            }
        }
    }

    // Fallback: random exploration within radius (prefer unvisited directions)
    for (int attempt = 0; attempt < 30; attempt++) {
        double r = (rand_range(1, 1000) / 1000.0) * radius;
        double theta = (rand_range(0, 628) / 100.0);
        int tx = g_home_x + static_cast<int>(std::round(r * std::cos(theta)));
        int ty = g_home_y + static_cast<int>(std::round(r * std::sin(theta)));

        if (dist(hx, hy, tx, ty) < 10) continue;
        // Skip if recently visited
        if (was_visited_recently(tx, ty)) continue;

        auto line = try_create_line(hx, hy, tx, ty);
        if (!line.empty() && line.size() > 5) {
            g_explore_line = line;
            g_explore_line_idx = 0;
            return true;
        }
    }
    return false;
}

// Follow the current line: find the farthest reachable point ahead and jump to it
inline bool follow_line(uintptr_t hero, int hx, int hy) {
    if (g_explore_line.empty()) return false;

    // Find where we are on the line (closest point)
    int closest_idx = 0;
    double closest_d = 999999.0;
    for (int i = 0; i < static_cast<int>(g_explore_line.size()); i++) {
        auto& [lx, ly] = g_explore_line[i];
        double d = dist(hx, hy, lx, ly);
        if (d < closest_d) { closest_d = d; closest_idx = i; }
    }

    // Look ahead from closest point, find the farthest jumpable cell
    int best_idx = -1;
    for (int i = static_cast<int>(g_explore_line.size()) - 1; i > closest_idx; i--) {
        auto& [lx, ly] = g_explore_line[i];
        if (pathfinder::can_jump_to(hx, hy, lx, ly) && !is_jump_blocked(lx, ly)) {
            best_idx = i;
            break;
        }
    }

    if (best_idx < 0) {
        // Can't progress along this line, abandon it
        g_explore_line.clear();
        return false;
    }

    auto& [tx, ty] = g_explore_line[best_idx];
    if (do_jump(hero, hx, hy, tx, ty)) {
        // Check if we're near the end
        if (best_idx >= static_cast<int>(g_explore_line.size()) - 3)
            g_explore_line.clear(); // line done
        return true;
    }

    // Jump failed (blocked zone), abandon line
    g_explore_line.clear();
    return false;
}

// Check if current line is still valid (we haven't drifted too far from it)
inline bool is_line_valid(int hx, int hy) {
    if (g_explore_line.empty()) return false;
    // Check if we're reasonably close to any point on the line
    for (auto& [lx, ly] : g_explore_line) {
        if (dist(hx, hy, lx, ly) <= MAX_JUMP) return true;
    }
    return false;
}

inline bool explore_tick(uintptr_t hero, int hx, int hy, int64_t now) {
    if (now - g_last_explore_time < EXPLORE_COOLDOWN_MS) return false;

    mark_visited(hx, hy);

    // If we have a valid line, follow it
    if (is_line_valid(hx, hy)) {
        if (follow_line(hero, hx, hy)) {
            g_last_explore_time = now;
            return true;
        }
    }

    // No line or line exhausted — pick a new destination
    if (pick_new_line(hx, hy)) {
        if (follow_line(hero, hx, hy)) {
            g_last_explore_time = now;
            return true;
        }
    }

    // All 30 attempts failed to find a walkable line — try going home
    if (dist(hx, hy, g_home_x, g_home_y) > MAX_JUMP) {
        auto pos = direct_jump(hx, hy, g_home_x, g_home_y, 0);
        if (do_jump(hero, hx, hy, pos.x, pos.y)) {
            g_last_explore_time = now;
            return true;
        }
    }

    return false;
}

// Run AWAY from a threat to maintain safe Chebyshev distance
// Returns the target position to run toward (away from threat)
inline game::CMyPos kite_away(int hx, int hy, int tx, int ty, int target_dist) {
    double away_angle = std::atan2(static_cast<double>(hy - ty), static_cast<double>(hx - tx));

    // Try the ideal away direction first, then fan out +/- if blocked
    for (int fan = 0; fan < 12; fan++) {
        double offset = (fan / 2) * 0.4 * (fan % 2 == 0 ? 1.0 : -1.0);
        double angle = away_angle + offset;

        // Use Chebyshev-aware positioning: place along the angle at target_dist
        int dest_x = tx + static_cast<int>(std::round(std::cos(angle) * target_dist * 1.2));
        int dest_y = ty + static_cast<int>(std::round(std::sin(angle) * target_dist * 1.2));

        if (dest_x == hx && dest_y == hy) continue;
        // Verify the destination actually achieves the desired game distance
        if (game_dist(tx, ty, dest_x, dest_y) < target_dist) continue;
        if (!pathfinder::is_blocked(dest_x, dest_y) && !is_jump_blocked(dest_x, dest_y))
            return {dest_x, dest_y};
    }
    return {hx, hy};
}

// Issue a run command toward (tx, ty). The game handles pathfinding to the destination.
inline void do_run(uintptr_t hero, int tx, int ty) {
    uintptr_t base = game::get_base();
    using WalkRun_t = void(__fastcall*)(uintptr_t, int, int);
    auto fn = reinterpret_cast<WalkRun_t>(base + game::FN_WALK_RUN);
    fn(hero, tx, ty);
}

// ═══════════════════════════════════════════════════════════════════════
// COMBAT MODE HELPERS - shared target finding used by melee/ranged/scatter
// ═══════════════════════════════════════════════════════════════════════

// Find target: try to keep current target, then find new one.
// If prefer_clusters is set, uses cluster scoring with stickiness.
// Returns nullptr if no valid target found.
inline const entities::EntityInfo* find_target(
    int hx, int hy,
    const std::vector<entities::EntityInfo>& nearby,
    int& out_gd)
{
    const entities::EntityInfo* target = nullptr;
    int best_gd = 9999;

    // Try to keep current target
    if (g_target_id != 0) {
        for (auto& e : nearby) {
            if (e.id == g_target_id && is_monster(e)) {
                target = &e;
                best_gd = game_dist(hx, hy, e.x, e.y);
                break;
            }
        }
    }

    if (!target) {
        if (g_config.prefer_clusters) {
            // Check if current cluster still has enough monsters to stay committed
            int cur_cluster_alive = 0;
            if (g_cluster_count > 0) {
                for (auto& e : nearby) {
                    if (!is_monster(e)) continue;
                    if (game_dist(e.x, e.y, g_cluster_cx, g_cluster_cy) <= g_config.cluster_radius + 2)
                        cur_cluster_alive++;
                }
            }

            bool stay_in_cluster = (g_cluster_count > 0
                && cur_cluster_alive >= g_cluster_count / 2
                && cur_cluster_alive >= g_config.cluster_min_stay);

            if (stay_in_cluster) {
                for (auto& e : nearby) {
                    if (!is_monster(e)) continue;
                    if (g_unreachable_targets.count(e.id)) continue;
                    if (game_dist(e.x, e.y, g_cluster_cx, g_cluster_cy) > g_config.cluster_radius + 2) continue;
                    int gd = game_dist(hx, hy, e.x, e.y);
                    if (gd < best_gd) { best_gd = gd; target = &e; }
                }
            }

            if (!target) {
                // Find best new cluster
                int best_score = -1;
                int best_cx = 0, best_cy = 0, best_count = 0;
                for (auto& e : nearby) {
                    if (!is_monster(e)) continue;
                    if (g_unreachable_targets.count(e.id)) continue;
                    int gd = game_dist(hx, hy, e.x, e.y);
                    int cluster = 0;
                    for (auto& other : nearby) {
                        if (&other == &e || !is_monster(other)) continue;
                        if (game_dist(e.x, e.y, other.x, other.y) <= g_config.cluster_radius)
                            cluster++;
                    }
                    int score = cluster * 100 - gd;
                    if (score > best_score) {
                        best_score = score;
                        best_gd = gd;
                        target = &e;
                        best_cx = e.x; best_cy = e.y;
                        best_count = cluster + 1;
                    }
                }
                if (target) {
                    g_cluster_cx = best_cx;
                    g_cluster_cy = best_cy;
                    g_cluster_count = best_count;
                }
            }
        } else {
            // Simple closest monster
            for (auto& e : nearby) {
                if (!is_monster(e)) continue;
                if (g_unreachable_targets.count(e.id)) continue;
                int gd = game_dist(hx, hy, e.x, e.y);
                if (gd < best_gd) { best_gd = gd; target = &e; }
            }
        }
    }

    out_gd = best_gd;
    return target;
}

// Find closest monster (simple, no cluster logic)
inline const entities::EntityInfo* find_closest_monster(
    int hx, int hy,
    const std::vector<entities::EntityInfo>& nearby,
    int& out_gd)
{
    const entities::EntityInfo* target = nullptr;
    int best_gd = 9999;

    // Try to keep current target
    if (g_target_id != 0) {
        for (auto& e : nearby) {
            if (e.id == g_target_id && is_monster(e)) {
                target = &e;
                best_gd = game_dist(hx, hy, e.x, e.y);
                break;
            }
        }
    }

    if (!target) {
        for (auto& e : nearby) {
            if (!is_monster(e)) continue;
            if (g_unreachable_targets.count(e.id)) continue;
            int gd = game_dist(hx, hy, e.x, e.y);
            if (gd < best_gd) { best_gd = gd; target = &e; }
        }
    }

    out_gd = best_gd;
    return target;
}

// Issue a lock-attack command on a target
inline void do_attack(uintptr_t hero, uint32_t target_id, int64_t now) {
    uint8_t cmd[0x108]{};
    *reinterpret_cast<int32_t*>(cmd + 0x00) = game::CMD_LOCKATK;
    *reinterpret_cast<uint32_t*>(cmd + 0x08) = target_id;
    call_set_command(hero, cmd);
    g_last_attack_time = now;
}

// Approach a target: try jump, fall back to pathfinder. Returns false if blacklisted.
inline bool approach_target(uintptr_t hero, int hx, int hy, int64_t now,
                            const entities::EntityInfo* target, int weapon_range) {
    auto pos = approach_jump_safe(hx, hy, target->x, target->y,
        g_config.jump_scatter, weapon_range);
    if (do_jump(hero, hx, hy, pos.x, pos.y))
        return true;
    // Jump failed — start pathfinder
    if (!pathfinder::is_pathfinding()) {
        pathfinder::start_pathfind(target->x, target->y);
        if (!pathfinder::is_pathfinding()) {
            // No path found — blacklist target
            g_target_id = 0;
            g_unreachable_targets[target->id] = now + 10000;
            return false;
        }
    }
    return true; // pathfinder will handle it
}

// ═══════════════════════════════════════════════════════════════════════
// MELEE TICK - hunt_mode 0
// ═══════════════════════════════════════════════════════════════════════

inline void melee_tick(uintptr_t hero, int hx, int hy, int64_t now,
                       const std::vector<entities::EntityInfo>& nearby,
                       int weapon_range)
{
    int best_gd = 9999;
    const entities::EntityInfo* target = find_target(hx, hy, nearby, best_gd);

    if (!target) {
        g_target_id = 0;
        explore_tick(hero, hx, hy, now);
        return;
    }
    g_target_id = target->id;

    // If pathfinding to a target, check if we're close enough to stop and fight
    if (pathfinder::is_pathfinding()) {
        if (best_gd <= weapon_range + g_config.action_range) {
            pathfinder::stop_pathfind();
        } else {
            return; // let pathfinder tick handle movement
        }
    }

    bool in_range = best_gd <= weapon_range + g_config.action_range;

    if (in_range) {
        // Check if already attacking this target
        int cur_type = 0, cur_status = 0;
        uint32_t cur_target = 0;
        game::safe_read<int>(hero + game::OFF_CMD_TYPE, cur_type);
        game::safe_read<int>(hero + game::OFF_CMD_STATUS, cur_status);
        game::safe_read<uint32_t>(hero + game::OFF_CMD_TARGET, cur_target);
        if (cur_type == game::CMD_LOCKATK && cur_target == target->id && cur_status != 6)
            return; // already attacking

        int atk_cd = game::is_cyclone() ? g_config.attack_cooldown_xp_ms : g_config.attack_cooldown_ms;
        if (now - g_last_attack_time < atk_cd) return;

        do_attack(hero, target->id, now);
    } else {
        // Out of range — approach
        approach_target(hero, hx, hy, now, target, weapon_range);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// RANGED TICK - hunt_mode 1
// ═══════════════════════════════════════════════════════════════════════

inline void ranged_tick(uintptr_t hero, int hx, int hy, int64_t now,
                        const std::vector<entities::EntityInfo>& nearby,
                        int weapon_range)
{
    int best_gd = 9999;
    const entities::EntityInfo* target = find_closest_monster(hx, hy, nearby, best_gd);

    if (!target) {
        g_target_id = 0;
        explore_tick(hero, hx, hy, now);
        return;
    }
    g_target_id = target->id;

    // If pathfinding, check if close enough
    if (pathfinder::is_pathfinding()) {
        if (best_gd <= weapon_range + g_config.action_range) {
            pathfinder::stop_pathfind();
        } else {
            return;
        }
    }

    // No need to kite when flying (hero is untargetable)
    bool too_close = g_config.kite_enabled && !game::is_flying()
                     && best_gd <= g_config.kite_min_dist;
    bool in_range = best_gd <= weapon_range + g_config.action_range;

    if (too_close) {
        // Kite: run away to safe distance
        int safe_dist = weapon_range;
        if (safe_dist < g_config.kite_min_dist + 2) safe_dist = g_config.kite_min_dist + 2;
        auto kite_pos = kite_away(hx, hy, target->x, target->y, safe_dist);
        if (kite_pos.x != hx || kite_pos.y != hy) {
            do_run(hero, kite_pos.x, kite_pos.y);
        }
        // Attack while kiting if cooldown ready
        int atk_cd = game::is_cyclone() ? g_config.attack_cooldown_xp_ms : g_config.attack_cooldown_ms;
        if (now - g_last_attack_time >= atk_cd) {
            do_attack(hero, target->id, now);
        }
    } else if (in_range) {
        // In range — attack
        int cur_type = 0, cur_status = 0;
        uint32_t cur_target = 0;
        game::safe_read<int>(hero + game::OFF_CMD_TYPE, cur_type);
        game::safe_read<int>(hero + game::OFF_CMD_STATUS, cur_status);
        game::safe_read<uint32_t>(hero + game::OFF_CMD_TARGET, cur_target);
        if (cur_type == game::CMD_LOCKATK && cur_target == target->id && cur_status != 6)
            return;

        int atk_cd = game::is_cyclone() ? g_config.attack_cooldown_xp_ms : g_config.attack_cooldown_ms;
        if (now - g_last_attack_time < atk_cd) return;

        do_attack(hero, target->id, now);
    } else {
        // Out of range — approach to weapon range
        approach_target(hero, hx, hy, now, target, weapon_range);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// SCATTER TICK - hunt_mode 2
// ═══════════════════════════════════════════════════════════════════════

inline void scatter_tick(uintptr_t hero, int hx, int hy, int64_t now,
                         const std::vector<entities::EntityInfo>& nearby,
                         int weapon_range)
{
    extern bool scatter_bot_tick_wrapper(uintptr_t hero, int hx, int hy, int64_t now);
    extern int scatter_find_best_position_wrapper(
        int hx, int hy, double range, double half_angle, int max_jump,
        int& out_x, int& out_y);

    // Count alive monsters
    int monster_count = 0;
    for (auto& e : nearby)
        if (is_monster(e)) monster_count++;

    int64_t cd_remaining = g_config.scatter_cooldown_ms - (now - g_last_scatter_time);
    if (cd_remaining < 0) cd_remaining = 0;

    // Always check if we can cast from current position first — interrupt any path
    if (scatter_bot_tick_wrapper(hero, hx, hy, now)) {
        if (pathfinder::is_pathfinding()) pathfinder::stop_pathfind();
        console::info("[SCATTER] Cast! %d monsters nearby", monster_count);
        return;
    }

    bool on_cooldown = (cd_remaining > 0);

    if (monster_count == 0) {
        if (pathfinder::is_pathfinding()) return; // let path finish
        console::info("[SCATTER] No monsters, exploring");
        g_target_id = 0;
        explore_tick(hero, hx, hy, now);
        return;
    }

    // Check current position and best reachable position
    int stay_x = hx, stay_y = hy;
    int here_predicted = scatter_find_best_position_wrapper(
        hx, hy, g_config.scatter_range, g_config.scatter_half_angle, 0,
        stay_x, stay_y);

    int move_x = hx, move_y = hy;
    int predicted = scatter_find_best_position_wrapper(
        hx, hy, g_config.scatter_range, g_config.scatter_half_angle,
        pathfinder::MAX_JUMP_DIST, move_x, move_y);

    // If pathfinding, interrupt if current position is already good enough
    if (pathfinder::is_pathfinding()) {
        if (here_predicted >= g_config.scatter_min_targets) {
            pathfinder::stop_pathfind();
            console::info("[SCATTER] Interrupted path — good spot here (%d hits)", here_predicted);
        }
        return; // let pathfinder tick handle movement
    }

    // If current spot is good and scatter ready, it'll fire next tick
    if (here_predicted >= g_config.scatter_min_targets && !on_cooldown)
        return;

    // Always keep moving toward the best position (don't idle during cooldown)
    if (predicted >= g_config.scatter_min_targets) {
        console::info("[SCATTER] Direct jump to (%d,%d) hits=%d", move_x, move_y, predicted);
        if (!do_jump(hero, hx, hy, move_x, move_y)) {
            // Direct jump failed — try bresenham line toward target
            auto line = trace_line(hx, hy, move_x, move_y);
            bool jumped = false;
            for (int i = (int)line.size() - 1; i > 0; i--) {
                auto& [lx, ly] = line[i];
                if (pathfinder::can_jump_to(hx, hy, lx, ly) && !is_jump_blocked(lx, ly)) {
                    do_jump(hero, hx, hy, lx, ly);
                    jumped = true;
                    break;
                }
            }
            if (!jumped) pathfinder::start_pathfind(move_x, move_y);
        }
        return;
    }

    // Not enough targets anywhere in jump range — find densest cluster and path NEAR it
    int best_cx = 0, best_cy = 0, best_count = 0;
    for (auto& e : nearby) {
        if (!is_monster(e)) continue;
        int count = 0;
        for (auto& o : nearby) {
            if (!is_monster(o)) continue;
            if (game_dist(e.x, e.y, o.x, o.y) <= (int)g_config.scatter_range)
                count++;
        }
        if (count > best_count) {
            best_count = count; best_cx = e.x; best_cy = e.y;
        }
    }

    if (best_count > 0) {
        // Path to a spot NEAR the cluster (scatter_range/2 away), not onto the monsters
        int range = (int)(g_config.scatter_range * 0.5);
        if (range < 3) range = 3;
        // Pick the side closest to hero
        double angle = std::atan2((double)(hy - best_cy), (double)(hx - best_cx));
        int dest_x = best_cx + (int)(std::cos(angle) * range);
        int dest_y = best_cy + (int)(std::sin(angle) * range);

        // Don't pathfind to our own position
        if (game_dist(hx, hy, dest_x, dest_y) <= 2) {
            // Already near the cluster — just explore to find denser area
            explore_tick(hero, hx, hy, now);
            return;
        }

        // Try direct line first
        auto line = trace_line(hx, hy, dest_x, dest_y);
        bool jumped = false;
        for (int i = (int)line.size() - 1; i > 0; i--) {
            auto& [lx, ly] = line[i];
            if (pathfinder::can_jump_to(hx, hy, lx, ly) && !is_jump_blocked(lx, ly)) {
                console::info("[SCATTER] Line jump toward cluster of %d at (%d,%d) -> (%d,%d)",
                    best_count, best_cx, best_cy, lx, ly);
                do_jump(hero, hx, hy, lx, ly);
                jumped = true;
                break;
            }
        }
        if (!jumped) {
            console::info("[SCATTER] Pathfinding near cluster of %d at (%d,%d) -> (%d,%d)",
                best_count, best_cx, best_cy, dest_x, dest_y);
            pathfinder::start_pathfind(dest_x, dest_y);
        }
    } else {
        explore_tick(hero, hx, hy, now);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// MAGIC TICK - hunt_mode 3 (single target magic kiting)
// ═══════════════════════════════════════════════════════════════════════

inline void magic_tick(uintptr_t hero, int hx, int hy, int64_t now,
                       const std::vector<entities::EntityInfo>& nearby,
                       int weapon_range)
{
    // Get the selected magic's info at hero's level
    auto magic_entry = game::find_magic(g_config.magic_id);
    const MagicTypeInfo* minfo = nullptr;
    if (magic_entry.id != 0)
        minfo = get_magic_info(g_config.magic_id, magic_entry.level);

    if (!minfo) {
        // Magic not learned or not in DB — fall back to explore
        static int64_t last_warn = 0;
        if (now - last_warn > 5000) {
            console::warn("[MAGIC] Magic %u not available (learned=%s lv=%u, db_key=%u)",
                g_config.magic_id, magic_entry.id ? "YES" : "NO", magic_entry.level,
                g_config.magic_id * 100 + magic_entry.level);
            last_warn = now;
        }
        explore_tick(hero, hx, hy, now);
        return;
    }

    int cast_range = minfo->distance;
    int mp_cost = minfo->mp_cost;
    int safe_dist = g_config.magic_safe_dist;

    // Check if any monster is too close — kite away first
    int closest_monster_dist = 9999;
    const entities::EntityInfo* closest_threat = nullptr;
    for (auto& e : nearby) {
        if (!is_monster(e)) continue;
        int gd = game_dist(hx, hy, e.x, e.y);
        if (gd < closest_monster_dist) {
            closest_monster_dist = gd;
            closest_threat = &e;
        }
    }

    if (closest_threat && closest_monster_dist <= safe_dist) {
        // Monster too close — kite away
        auto kite_pos = kite_away(hx, hy, closest_threat->x, closest_threat->y, safe_dist + 2);
        if (kite_pos.x != hx || kite_pos.y != hy) {
            do_jump(hero, hx, hy, kite_pos.x, kite_pos.y);
        }
        return;
    }

    // Find target: closest monster within cast range (or closest overall to approach)
    int best_gd = 9999;
    const entities::EntityInfo* target = nullptr;

    // Prefer current target if still valid
    if (g_target_id != 0) {
        for (auto& e : nearby) {
            if (e.id == g_target_id && is_monster(e)) {
                target = &e;
                best_gd = game_dist(hx, hy, e.x, e.y);
                break;
            }
        }
    }

    // Find new target if current is gone
    if (!target) {
        for (auto& e : nearby) {
            if (!is_monster(e)) continue;
            if (g_unreachable_targets.count(e.id)) continue;
            int gd = game_dist(hx, hy, e.x, e.y);
            if (gd < best_gd) { best_gd = gd; target = &e; }
        }
    }

    if (!target) {
        g_target_id = 0;
        explore_tick(hero, hx, hy, now);
        return;
    }
    g_target_id = target->id;

    // If pathfinding, check if we're close enough to stop
    if (pathfinder::is_pathfinding()) {
        if (best_gd <= cast_range) pathfinder::stop_pathfind();
        else return; // let pathfinder handle movement
    }

    // Check if target is in cast range
    if (best_gd <= cast_range) {
        // In range — check MP and cast delay
        int mp = game::get_mp();
        if (mp < mp_cost) {
            // Not enough MP — potion system handles this at PRIORITY 1b
            // Just wait here
            return;
        }

        // Check cast delay
        if (now - g_last_magic_cast_time < g_config.magic_cast_delay_ms)
            return;

        // Cast magic on target (MsgInteract with MAGIC_ATTACK action=21)
        uint32_t hero_id = 0;
        game::safe_read<uint32_t>(hero + game::OFF_ID, hero_id);

        auto write_varint = [](uint8_t* dst, uint32_t val) -> int {
            int n = 0;
            while (val > 0x7F) { dst[n++] = (val & 0x7F) | 0x80; val >>= 7; }
            dst[n++] = val & 0x7F;
            return n;
        };

        uint8_t pb[128];
        int pos = 0;
        pb[pos++] = 0x08; pos += write_varint(pb + pos, hero_id);          // playerId
        pb[pos++] = 0x10; pos += write_varint(pb + pos, target->id);       // targetId
        pb[pos++] = 0x18; pos += write_varint(pb + pos, (uint32_t)hx);     // x
        pb[pos++] = 0x20; pos += write_varint(pb + pos, (uint32_t)hy);     // y
        pb[pos++] = 0x30; pos += write_varint(pb + pos, 21);               // action = MAGIC_ATTACK
        pb[pos++] = 0x38; pos += write_varint(pb + pos, 0);                // damage
        pb[pos++] = 0x40; pos += write_varint(pb + pos, g_config.magic_id); // spellId
        pb[pos++] = 0x48; pos += write_varint(pb + pos, 0);                // koCount

        uint16_t total_size = static_cast<uint16_t>(4 + pos);
        uint16_t msg_type = 1022; // MsgInteract
        uint8_t packet[256];
        memcpy(packet, &total_size, 2);
        memcpy(packet + 2, &msg_type, 2);
        memcpy(packet + 4, pb, pos);

        uintptr_t b = game::get_base();
        using GetMgr_t = uintptr_t(__fastcall*)();
        auto get_mgr = reinterpret_cast<GetMgr_t>(b + game::FN_GET_SEND_MGR);
        uintptr_t mgr = get_mgr();
        if (mgr) {
            uintptr_t net_buf = 0;
            game::safe_read(mgr + 32, net_buf);
            if (net_buf) {
                using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                auto raw_send = reinterpret_cast<RawSend_t>(b + 0x1DBA40);
                raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size));
            }
        }

        g_last_magic_cast_time = now;
    } else {
        // Out of range — approach to cast range while staying safe
        // Target a position at cast_range-2 distance from monster (not on top)
        int approach_range = cast_range - 2;
        if (approach_range < 1) approach_range = 1;

        double angle = std::atan2((double)(hy - target->y), (double)(hx - target->x));
        int dest_x = target->x + (int)(std::cos(angle) * approach_range);
        int dest_y = target->y + (int)(std::sin(angle) * approach_range);

        if (!do_jump(hero, hx, hy, dest_x, dest_y)) {
            if (!pathfinder::is_pathfinding())
                pathfinder::start_pathfind(target->x, target->y);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TICK - called from hooked_process on game thread every frame
// ═══════════════════════════════════════════════════════════════════════

inline void tick(uintptr_t hero) {
    if (!g_running.load()) return;
    if (!hero) return;

    if (game::is_dead() || game::is_ghost()) return;

    // Check if hero is busy (movement/attack animation)
    {
        int anim = 0;
        game::safe_read<int>(hero + 0x118, anim);
        switch (anim) {
            case 110: case 115: case 120: case 121: case 125: case 126:
            case 130: case 131: case 132: case 140: case 320: case 470: case 510:
                return;
        }
    }

    int64_t now = static_cast<int64_t>(GetTickCount64());

    // Check if hero is busy (movement/attack animation)
    {
        int anim = 0;
        game::safe_read<int>(hero + 0x118, anim);
        switch (anim) {
            case 110: case 115: case 120: case 121: case 125: case 126:
            case 130: case 131: case 132: case 140: case 320: case 470: case 510:
                return;
        }
    }

    // Baseline 50ms between ticks to not spam the game thread
    if (now - g_last_tick_time < 50) return;
    g_last_tick_time = now;

    int hx = 0, hy = 0;
    game::safe_read<int>(hero + game::OFF_POS_X, hx);
    game::safe_read<int>(hero + game::OFF_POS_Y, hy);
    if (hx <= 0 || hy <= 0) return;

    // ── Jump rollback detection ──────────────────────────────────────
    if (g_last_jump.time > 0 && now - g_last_jump.time > 300 && now - g_last_jump.time < 2000) {
        if (hx == g_last_jump.from_x && hy == g_last_jump.from_y) {
            // Rolled back! Escalate: each consecutive rollback widens the blocked zone
            g_consecutive_rollbacks++;
            int zone_radius = 3 + g_consecutive_rollbacks * 2; // 5, 7, 9, 11...
            if (zone_radius > 15) zone_radius = 15;
            int64_t duration = 30000 + g_consecutive_rollbacks * 15000; // 45s, 60s, 75s...
            if (duration > 120000) duration = 120000;

            g_blocked_zones.push_back({
                g_last_jump.to_x, g_last_jump.to_y,
                zone_radius, now + duration
            });
            console::warn("[ANTI-STUCK] Rollback #%d: blocked zone r=%d at (%d,%d) for %ds",
                g_consecutive_rollbacks, zone_radius,
                g_last_jump.to_x, g_last_jump.to_y,
                static_cast<int>(duration / 1000));
            g_last_jump.time = 0;
        } else {
            // Jump succeeded - reset consecutive counter
            g_consecutive_rollbacks = 0;
            g_last_jump.time = 0;
        }
    }
    // Expire old blocked zones
    for (auto it = g_blocked_zones.begin(); it != g_blocked_zones.end(); ) {
        if (now > it->expiry) it = g_blocked_zones.erase(it);
        else ++it;
    }

    uintptr_t base = game::get_base();
    int weapon_range = get_weapon_attack_range();
    double attack_range = static_cast<double>(weapon_range);
    if (attack_range < 1.0) attack_range = 1.0;
    double action_range = attack_range + static_cast<double>(g_config.action_range);
    double pickup_range = action_range;

    // ── Early entity scan (shared by safety + combat) ────────────────
    auto nearby = fast_scan_nearby();

    // ── PRIORITY -1: Player safety ────────────────────────────────────
    player_safety::tick(hero, nearby);
    if (player_safety::is_busy()) return; // safety system has control

    // ── PRIORITY 0: Auto-deposit check ─────────────────────────────────
    if (g_config.auto_deposit && !auto_deposit::is_active()) {
        if (auto_deposit::should_auto_deposit(g_config.deposit_threshold)) {
            console::info("[BOT] Inventory full (%d items), starting auto-deposit", g_config.deposit_threshold);
            auto_deposit::start(g_config.deposit_threshold);
            return;
        }
    }

    // ── PRIORITY 1: Auto HP potion ──────────────────────────────────────
    if (g_config.auto_hp_pot) {
        int hp = game::get_hp();
        int max_hp = game::get_max_hp();
        if (hp > 0 && max_hp > 0 && hp < max_hp) {
            int deficit = max_hp - hp;
            bool critical = (deficit >= max_hp * 4 / 5);

            auto inv = game::get_inventory();
            uint32_t best_item_id = 0;
            int best_heal = 0;

            if (critical) {
                int best_diff = INT_MAX;
                for (auto& p : g_config.potions) {
                    int heal = static_cast<int>(p.heal_amount);
                    int diff = std::abs(heal - deficit);
                    if (diff >= best_diff) continue;
                    for (auto& it : inv) {
                        if (it.type_id == p.type_id && it.amount > 0) {
                            best_diff = diff;
                            best_heal = heal;
                            best_item_id = it.item_id;
                            break;
                        }
                    }
                }
            } else {
                for (auto& p : g_config.potions) {
                    int heal = static_cast<int>(p.heal_amount);
                    if (heal > deficit) continue;
                    if (heal <= best_heal) continue;
                    for (auto& it : inv) {
                        if (it.type_id == p.type_id && it.amount > 0) {
                            best_item_id = it.item_id;
                            best_heal = heal;
                            break;
                        }
                    }
                }
            }
            if (best_item_id) {
                using UseItem_t = void(__fastcall*)(uintptr_t, unsigned int);
                auto fn = reinterpret_cast<UseItem_t>(base + game::FN_USE_ITEM);
                fn(hero, best_item_id);
                return;
            }
        }
    }

    // ── PRIORITY 1b: Auto MP potion ─────────────────────────────────────
    if (g_config.auto_mp_pot && !g_config.mp_potions.empty()) {
        int mp = game::get_mp();
        int max_mp = game::get_max_mp();
        if (mp >= 0 && max_mp > 0 && (float)mp / max_mp < g_config.mp_potion_pct) {
            int deficit = max_mp - mp;
            auto inv = game::get_inventory();
            uint32_t best_item_id = 0;
            int best_mana = 0;

            bool critical = ((float)mp / max_mp < 0.2f);
            if (critical) {
                // Emergency: pick closest to deficit
                int best_diff = INT_MAX;
                for (auto& p : g_config.mp_potions) {
                    int heal = static_cast<int>(p.mana_amount);
                    int diff = std::abs(heal - deficit);
                    if (diff >= best_diff) continue;
                    for (auto& it : inv) {
                        if (it.type_id == p.type_id && it.amount > 0) {
                            best_diff = diff;
                            best_mana = heal;
                            best_item_id = it.item_id;
                            break;
                        }
                    }
                }
            } else {
                // Non-critical: use potion that wastes no mana
                for (auto& p : g_config.mp_potions) {
                    int heal = static_cast<int>(p.mana_amount);
                    if (heal > deficit) continue;
                    if (heal <= best_mana) continue;
                    for (auto& it : inv) {
                        if (it.type_id == p.type_id && it.amount > 0) {
                            best_item_id = it.item_id;
                            best_mana = heal;
                            break;
                        }
                    }
                }
            }
            if (best_item_id) {
                using UseItem_t = void(__fastcall*)(uintptr_t, unsigned int);
                auto fn = reinterpret_cast<UseItem_t>(base + game::FN_USE_ITEM);
                fn(hero, best_item_id);
                return;
            }
        }
    }

    // ── PRIORITY 2: XP Skill (Cyclone, Fly, Superman, etc) ─────────────
    if (g_config.use_xp_skill && g_has_xp_skill && game::is_xp_full()) {
        if (now - g_last_xp_skill_time >= g_config.xp_skill_delay_ms) {
            uint32_t hero_id = 0;
            game::safe_read<uint32_t>(hero + game::OFF_ID, hero_id);

            auto write_varint = [](uint8_t* dst, uint32_t val) -> int {
                int n = 0;
                while (val > 0x7F) { dst[n++] = (val & 0x7F) | 0x80; val >>= 7; }
                dst[n++] = val & 0x7F;
                return n;
            };

            uint8_t pb[128];
            int pos = 0;
            pb[pos++] = 0x08; pos += write_varint(pb + pos, hero_id);
            pb[pos++] = 0x10; pos += write_varint(pb + pos, hero_id);
            pb[pos++] = 0x18; pos += write_varint(pb + pos, (uint32_t)hx);
            pb[pos++] = 0x20; pos += write_varint(pb + pos, (uint32_t)hy);
            pb[pos++] = 0x30; pos += write_varint(pb + pos, 21); // MAGIC_ATTACK
            pb[pos++] = 0x38; pos += write_varint(pb + pos, 0);
            pb[pos++] = 0x40; pos += write_varint(pb + pos, g_config.xp_skill_id);
            pb[pos++] = 0x48; pos += write_varint(pb + pos, 0);

            uint16_t total_size = static_cast<uint16_t>(4 + pos);
            uint16_t msg_type = 1022;
            uint8_t packet[256];
            memcpy(packet, &total_size, 2);
            memcpy(packet + 2, &msg_type, 2);
            memcpy(packet + 4, pb, pos);

            using GetMgr_t = uintptr_t(__fastcall*)();
            auto get_mgr = reinterpret_cast<GetMgr_t>(base + game::FN_GET_SEND_MGR);
            uintptr_t mgr = get_mgr();
            if (mgr) {
                uintptr_t net_buf = 0;
                game::safe_read(mgr + 32, net_buf);
                if (net_buf) {
                    using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
                    auto raw_send = reinterpret_cast<RawSend_t>(base + 0x1DBA40);
                    raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size));
                    console::info("[BOT] XP Skill %u activated!", g_config.xp_skill_id);
                }
            }
            g_last_xp_skill_time = now;

            return;
        }
    }

    // ── PRIORITY 2.5: Meteor auto-pack (VIP only) ────────────────────
    if (now - g_last_meteor_check >= 5000) { // check every 5 seconds
        g_last_meteor_check = now;
        int vip = 0;
        game::safe_read<int>(hero + game::OFF_VIP, vip);
        if (vip) {
            auto inv = game::get_inventory();
            int meteor_count = 0;
            uint32_t meteor_instance_id = 0;
            for (auto& it : inv) {
                if (it.type_id == METEOR_TYPE_ID) {
                    meteor_count++; // each meteor is 1 slot (non-stackable)
                    if (!meteor_instance_id) meteor_instance_id = it.item_id;
                }
            }
            if (meteor_count >= METEOR_PACK_COUNT && meteor_instance_id) {
                console::info("[BOT] Auto-packing %d meteors into scroll", meteor_count);
                game::queue_use_item(meteor_instance_id);

                return;
            }
        }
    }

    // ── PRIORITY 3: Loot items ──────────────────────────────────────────
    {
        int64_t since_pickup = now - g_last_pickup_time;
        if (since_pickup >= g_config.pickup_cooldown_ms) {
            auto items = fast_scan_ground_items();
            if (!items.empty()) {
                // Count HP and MP potions separately
                bool need_hp_pots = false, need_mp_pots = false;
                {
                    auto inv = game::get_inventory();
                    int hp_pot_count = 0, mp_pot_count = 0;
                    for (auto& it : inv) {
                        if (is_hp_potion_type(it.type_id)) hp_pot_count += it.amount;
                        if (is_mp_potion_type(it.type_id)) mp_pot_count += it.amount;
                    }
                    int threshold = static_cast<int>(g_config.pickup_potion_threshold);
                    need_hp_pots = (hp_pot_count < threshold);
                    need_mp_pots = (mp_pot_count < threshold);
                }

                for (auto it = g_pickup_blacklist.begin(); it != g_pickup_blacklist.end(); ) {
                    if (now > it->second) it = g_pickup_blacklist.erase(it);
                    else ++it;
                }
                {
                    std::unordered_map<uint32_t, bool> on_ground;
                    for (auto& gi : items) on_ground[gi.item_id] = true;
                    for (auto it = g_pickup_attempts.begin(); it != g_pickup_attempts.end(); ) {
                        if (!on_ground.count(it->first)) it = g_pickup_attempts.erase(it);
                        else ++it;
                    }
                    for (auto it = g_item_notice_time.begin(); it != g_item_notice_time.end(); ) {
                        if (!on_ground.count(it->first)) it = g_item_notice_time.erase(it);
                        else ++it;
                    }
                }

                double best_d = 999999.0;
                const entities::GroundItem* best = nullptr;
                for (auto& gi : items) {
                    if (g_pickup_blacklist.count(gi.item_id)) continue;
                    auto notice_it = g_item_notice_time.find(gi.item_id);
                    if (notice_it == g_item_notice_time.end()) {
                        int delay = rand_range(g_config.item_notice_min_ms, g_config.item_notice_max_ms);
                        g_item_notice_time[gi.item_id] = now + delay;
                        continue;
                    }
                    if (now < notice_it->second) continue;
                    double d = dist(hx, hy, gi.x, gi.y);
                    if (d >= best_d) continue;
                    if (is_loot_eligible(gi, need_hp_pots, need_mp_pots)) { best_d = d; best = &gi; }
                }

                if (best) {
                    int gd = game_dist(hx, hy, best->x, best->y);
                    if (gd <= g_config.action_range) {
                        // In range — pick it up
                        if (pathfinder::is_pathfinding()) pathfinder::stop_pathfind();
                        int& attempts = g_pickup_attempts[best->item_id];
                        attempts++;
                        if (attempts > MAX_PICKUP_ATTEMPTS) {
                            g_pickup_blacklist[best->item_id] = now + 60000;
                            g_pickup_attempts.erase(best->item_id);
                            return;
                        }
                        uint8_t cmd[0x108]{};
                        *reinterpret_cast<int32_t*>(cmd + 0x00) = game::CMD_PICKUP;
                        *reinterpret_cast<uint32_t*>(cmd + 0x08) = best->item_id;
                        *reinterpret_cast<int32_t*>(cmd + 0x0C) = best->x;
                        *reinterpret_cast<int32_t*>(cmd + 0x10) = best->y;
                        call_set_command(hero, cmd);
                        g_last_pickup_time = now;
                        return;
                    } else {
                        // Out of range — pathfind to the item
                        // Use start_pathfind so pathfinder::is_pathfinding() is true
                        // which prevents combat from interfering
                        if (!pathfinder::is_pathfinding()) {
                            pathfinder::start_pathfind(best->x, best->y);
                            if (!pathfinder::is_pathfinding()) {
                                // Pathfinding failed — blacklist
                                g_pickup_blacklist[best->item_id] = now + 15000;
                            }
                        }
                        return; // let pathfinder tick handle movement
                    }
                }
            }
        }
    }

    // ── PRIORITY 4: Combat dispatch ─────────────────────────────────────
    {
        // Don't start combat while pickup cooldown is active (give pickup time to complete)
        if (now - g_last_pickup_time < g_config.pickup_cooldown_ms) return;

        // Clean expired unreachable blacklist
        for (auto it = g_unreachable_targets.begin(); it != g_unreachable_targets.end(); ) {
            if (now > it->second) it = g_unreachable_targets.erase(it);
            else ++it;
        }

        // If pathfinder is active (from pickup or travel), let it finish
        if (pathfinder::is_pathfinding()) return;

        // Reuse entity list from early scan
        switch (g_config.hunt_mode) {
            case 0: melee_tick(hero, hx, hy, now, nearby, weapon_range); break;
            case 1: ranged_tick(hero, hx, hy, now, nearby, weapon_range); break;
            case 2: scatter_tick(hero, hx, hy, now, nearby, weapon_range); break;
            case 3: magic_tick(hero, hx, hy, now, nearby, weapon_range); break;
            default: melee_tick(hero, hx, hy, now, nearby, weapon_range); break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// START / STOP
// ═══════════════════════════════════════════════════════════════════════

inline void load_and_start(const std::string& cfg_path, const std::string& item_path) {
    BotConfig cfg;

    // Load bot config JSON
    try {
        std::ifstream f(cfg_path);
        if (f.is_open()) {
            nlohmann::json j;
            f >> j;
            if (j.contains("hp_pct")) cfg.hp_potion_pct = j["hp_pct"].get<float>();
            if (j.contains("auto_hp_pot")) cfg.auto_hp_pot = j["auto_hp_pot"].get<bool>();
            if (j.contains("auto_mp_pot")) cfg.auto_mp_pot = j["auto_mp_pot"].get<bool>();
            if (j.contains("mp_pct")) cfg.mp_potion_pct = j["mp_pct"].get<float>();
            if (j.contains("potion_threshold")) cfg.pickup_potion_threshold = j["potion_threshold"].get<uint32_t>();
            if (j.contains("reaction_min")) cfg.reaction_min_ms = j["reaction_min"].get<int>();
            if (j.contains("reaction_max")) cfg.reaction_max_ms = j["reaction_max"].get<int>();
            if (j.contains("attack_cd")) cfg.attack_cooldown_ms = j["attack_cd"].get<int>();
            if (j.contains("pickup_cd")) cfg.pickup_cooldown_ms = j["pickup_cd"].get<int>();
            if (j.contains("loot_plus")) cfg.loot_plus = j["loot_plus"].get<bool>();
            if (j.contains("loot_min_quality")) cfg.loot_min_quality = j["loot_min_quality"].get<int>();
            if (j.contains("monster_filter_mode")) cfg.monster_filter_mode = j["monster_filter_mode"].get<int>();
            if (j.contains("monster_filter_names")) {
                cfg.monster_filter_names.clear();
                for (auto& n : j["monster_filter_names"])
                    cfg.monster_filter_names.push_back(n.get<std::string>());
            }
            if (j.contains("action_range")) cfg.action_range = j["action_range"].get<int>();
            if (j.contains("jump_scatter")) cfg.jump_scatter = j["jump_scatter"].get<int>();
            if (j.contains("item_notice_min")) cfg.item_notice_min_ms = j["item_notice_min"].get<int>();
            if (j.contains("item_notice_max")) cfg.item_notice_max_ms = j["item_notice_max"].get<int>();
            if (j.contains("auto_deposit")) cfg.auto_deposit = j["auto_deposit"].get<bool>();
            if (j.contains("deposit_threshold")) cfg.deposit_threshold = j["deposit_threshold"].get<int>();
            if (j.contains("use_xp_skill")) cfg.use_xp_skill = j["use_xp_skill"].get<bool>();
            else if (j.contains("use_cyclone")) cfg.use_xp_skill = j["use_cyclone"].get<bool>(); // compat
            if (j.contains("xp_skill_id")) cfg.xp_skill_id = j["xp_skill_id"].get<uint32_t>();
            else if (j.contains("cyclone_magic_id")) cfg.xp_skill_id = j["cyclone_magic_id"].get<uint32_t>(); // compat
            if (j.contains("xp_skill_delay")) cfg.xp_skill_delay_ms = j["xp_skill_delay"].get<int>();
            else if (j.contains("cyclone_delay")) cfg.xp_skill_delay_ms = j["cyclone_delay"].get<int>(); // compat
            if (j.contains("attack_cd_xp")) cfg.attack_cooldown_xp_ms = j["attack_cd_xp"].get<int>();
            else if (j.contains("attack_cd_cyclone")) cfg.attack_cooldown_xp_ms = j["attack_cd_cyclone"].get<int>(); // compat
            if (j.contains("kite_enabled")) cfg.kite_enabled = j["kite_enabled"].get<bool>();
            if (j.contains("kite_min_dist")) cfg.kite_min_dist = j["kite_min_dist"].get<int>();
            if (j.contains("hunt_mode")) cfg.hunt_mode = j["hunt_mode"].get<int>();
            if (j.contains("pickup_hp_pots")) cfg.pickup_hp_pots = j["pickup_hp_pots"].get<bool>();
            if (j.contains("pickup_mp_pots")) cfg.pickup_mp_pots = j["pickup_mp_pots"].get<bool>();
            if (j.contains("magic_id")) cfg.magic_id = j["magic_id"].get<uint32_t>();
            if (j.contains("magic_cast_delay")) cfg.magic_cast_delay_ms = j["magic_cast_delay"].get<int>();
            if (j.contains("magic_safe_dist")) cfg.magic_safe_dist = j["magic_safe_dist"].get<int>();
            if (j.contains("prefer_clusters")) cfg.prefer_clusters = j["prefer_clusters"].get<bool>();
            if (j.contains("cluster_radius")) cfg.cluster_radius = j["cluster_radius"].get<int>();
            if (j.contains("cluster_min_stay")) cfg.cluster_min_stay = j["cluster_min_stay"].get<int>();
            if (j.contains("use_scatter")) cfg.use_scatter = j["use_scatter"].get<bool>();
            if (j.contains("scatter_magic_id")) cfg.scatter_magic_id = j["scatter_magic_id"].get<uint32_t>();
            if (j.contains("scatter_cooldown")) cfg.scatter_cooldown_ms = j["scatter_cooldown"].get<int>();
            if (j.contains("scatter_range")) cfg.scatter_range = j["scatter_range"].get<double>();
            if (j.contains("scatter_half_angle")) cfg.scatter_half_angle = j["scatter_half_angle"].get<double>();
            if (j.contains("scatter_min_targets")) cfg.scatter_min_targets = j["scatter_min_targets"].get<int>();
            if (j.contains("scatter_predict_hits")) cfg.scatter_predict_hits = j["scatter_predict_hits"].get<bool>();
            if (j.contains("explore_radius")) cfg.explore_radius = j["explore_radius"].get<int>();
            if (j.contains("pickup_types")) {
                for (auto& v : j["pickup_types"]) cfg.pickup_types.push_back(v.get<uint32_t>());
            }
            // Player safety
            if (j.contains("safety_enabled")) cfg.safety_enabled = j["safety_enabled"].get<bool>();
            if (j.contains("safety_flee_distance")) cfg.safety_flee_distance = j["safety_flee_distance"].get<int>();
            if (j.contains("safety_max_encounters")) cfg.safety_max_encounters = j["safety_max_encounters"].get<int>();
            if (j.contains("safety_encounter_window")) cfg.safety_encounter_window = j["safety_encounter_window"].get<int>();
            if (j.contains("safety_player_action")) cfg.safety_player_action = j["safety_player_action"].get<int>();
            if (j.contains("safety_safe_x")) cfg.safety_safe_x = j["safety_safe_x"].get<int>();
            if (j.contains("safety_safe_y")) cfg.safety_safe_y = j["safety_safe_y"].get<int>();
            if (j.contains("safety_safe_map")) cfg.safety_safe_map = j["safety_safe_map"].get<uint32_t>();
            if (j.contains("safety_wait_min")) cfg.safety_wait_min = j["safety_wait_min"].get<int>();
            if (j.contains("safety_wait_max")) cfg.safety_wait_max = j["safety_wait_max"].get<int>();
            if (j.contains("safety_gm_detect")) cfg.safety_gm_detect = j["safety_gm_detect"].get<bool>();
            if (j.contains("safety_gm_action")) cfg.safety_gm_action = j["safety_gm_action"].get<int>();
            if (j.contains("safety_gm_wait_min")) cfg.safety_gm_wait_min = j["safety_gm_wait_min"].get<int>();
            if (j.contains("safety_gm_wait_max")) cfg.safety_gm_wait_max = j["safety_gm_wait_max"].get<int>();
            if (j.contains("safety_gm_sound")) cfg.safety_gm_sound = j["safety_gm_sound"].get<bool>();
            console::ok("[BOT] Config loaded: hp=%.0f%% pickups=%d", cfg.hp_potion_pct * 100.0f, (int)cfg.pickup_types.size());
        } else {
            console::warn("[BOT] Config file not found: %s", cfg_path.c_str());
        }
    } catch (const std::exception& e) {
        console::error("[BOT] Config parse error: %s", e.what());
    }

    // Load itemtype database from itemtype.json (potions + attack ranges)
    g_itemtype_db.clear();
    try {
        std::ifstream f(item_path);
        if (f.is_open()) {
            nlohmann::json data = nlohmann::json::parse(f);
            for (auto& item : data) {
                if (!item.contains("id")) continue;
                uint32_t id = item["id"].get<uint32_t>();

                // HP Potions (items with life > 0 in potion range 1000000-1099999)
                uint32_t life = item.value("life", 0u);
                if (life > 0 && id >= 1000000 && id < 1100000)
                    cfg.potions.push_back({id, life});

                // MP Potions (items with mana > 0)
                uint32_t mana = item.value("mana", 0u);
                if (mana > 0)
                    cfg.mp_potions.push_back({id, mana});

                // Item type info (attack range)
                uint16_t atk_range = static_cast<uint16_t>(item.value("attackRange", 0));
                if (atk_range == 0)
                    atk_range = static_cast<uint16_t>(item.value("attack_range", 0));
                g_itemtype_db[id] = {id, atk_range};
            }
            console::ok("[BOT] Loaded %d item types (%d hp pots, %d mp pots) from itemtype.json",
                (int)g_itemtype_db.size(), (int)cfg.potions.size(), (int)cfg.mp_potions.size());
        } else {
            console::warn("[BOT] itemtype.json not found: %s", item_path.c_str());
        }
    } catch (const std::exception& e) {
        console::error("[BOT] itemtype.json parse error: %s", e.what());
    }

    // Load magictype.json for magic hunt mode
    g_magictype_db.clear();
    try {
        // Derive magictype.json path from item_path (same directory)
        auto magic_path = std::filesystem::path(item_path).parent_path() / "magictype.json";
        std::ifstream mf(magic_path);
        if (mf.is_open()) {
            auto mdata = nlohmann::json::parse(mf);
            for (auto& m : mdata) {
                MagicTypeInfo mi;
                mi.magic_type = m.value("MagicType", 0u);
                mi.level = m.value("Level", 0);
                mi.action_sort = m.value("ActionSort", 0);
                mi.distance = m.value("Distance", 0);
                mi.mp_cost = m.value("MpCost", 0);
                mi.power = m.value("Power", 0);
                mi.is_xp = m.value("Xp", 0) != 0;
                mi.name = m.value("Name", "");
                g_magictype_db[mi.magic_type * 100 + mi.level] = mi;
            }
            console::ok("[BOT] Loaded %d magic type entries from magictype.json", (int)g_magictype_db.size());
        }
    } catch (...) {}

    // Scan hero's known magics
    g_hero_magics = game::get_magic_list();
    console::info("[BOT] Hero has %d magics", (int)g_hero_magics.size());
    for (auto& m : g_hero_magics) {
        auto* mi = get_magic_info(m.id, m.level);
        if (mi && mi->action_sort == 1 && !mi->is_xp) {
            console::info("[BOT]   Magic: %s (id=%u lv=%u dist=%d mp=%d pow=%d)",
                mi->name.c_str(), mi->magic_type, mi->level, mi->distance, mi->mp_cost, mi->power);
        }
    }

    g_config = cfg;
    g_last_magic_cast_time = 0;
    g_target_id = 0;
    g_last_attack_time = 0;
    g_last_pickup_time = 0;
    g_last_tick_time = 0;
    g_last_xp_skill_time = 0;
    g_last_scatter_time = 0;
    g_pickup_blacklist.clear();
    g_pickup_attempts.clear();
    g_item_notice_time.clear();
    g_last_jump = {};
    g_blocked_zones.clear();
    g_consecutive_rollbacks = 0;
    // Set home position for exploration
    {
        uintptr_t h = game::get_hero();
        if (h) {
            game::safe_read<int>(h + game::OFF_POS_X, g_home_x);
            game::safe_read<int>(h + game::OFF_POS_Y, g_home_y);
        }
    }
    g_explore_index = 0;
    g_explore_line.clear();
    g_explore_line_idx = 0;
    g_visited.clear();
    g_last_explore_time = 0;
    // Check if the hero has the configured magics
    g_has_xp_skill = (game::find_magic(cfg.xp_skill_id).id != 0);
    g_has_scatter_magic = (game::find_magic(cfg.scatter_magic_id).id != 0);

    // Configure player safety
    {
        player_safety::Config sc;
        sc.enabled = cfg.safety_enabled;
        sc.flee_distance = cfg.safety_flee_distance;
        sc.max_encounters = cfg.safety_max_encounters;
        sc.encounter_window_sec = cfg.safety_encounter_window;
        sc.player_action = static_cast<player_safety::PlayerAction>(cfg.safety_player_action);
        sc.safe_x = cfg.safety_safe_x;
        sc.safe_y = cfg.safety_safe_y;
        sc.safe_map = cfg.safety_safe_map;
        sc.wait_min_sec = cfg.safety_wait_min;
        sc.wait_max_sec = cfg.safety_wait_max;
        sc.gm_detect = cfg.safety_gm_detect;
        sc.gm_action = static_cast<player_safety::GmAction>(cfg.safety_gm_action);
        sc.gm_wait_min_sec = cfg.safety_gm_wait_min;
        sc.gm_wait_max_sec = cfg.safety_gm_wait_max;
        sc.gm_play_sound = cfg.safety_gm_sound;
        player_safety::configure(sc);
        player_safety::reset();
        player_safety::set_origin(g_home_x, g_home_y, game::get_map_id());
        if (sc.enabled)
            console::ok("[BOT] Player safety ON (action=%d max_enc=%d gm=%s sound=%s)",
                cfg.safety_player_action, sc.max_encounters,
                sc.gm_detect ? "YES" : "NO", sc.gm_play_sound ? "YES" : "NO");
    }

    g_running.store(true);

    int wep_range = get_weapon_attack_range();
    uint32_t wep_type = game::get_weapon_type();
    console::ok("[BOT] Weapon type=%u atk_range=%d action_range=%d xp_skill(%u)=%s scatter=%s",
        wep_type, wep_range, cfg.action_range,
        cfg.xp_skill_id, g_has_xp_skill ? "YES" : "NO",
        g_has_scatter_magic ? "YES" : "NO");

    const char* mode_names[] = {"melee", "ranged", "scatter", "magic"};
    const char* mode_name = (cfg.hunt_mode >= 0 && cfg.hunt_mode <= 3) ? mode_names[cfg.hunt_mode] : "unknown";
    console::ok("[BOT] Started (mode=%s hp=%.0f%% atk_cd=%dms pickup_cd=%dms potions=%d)",
        mode_name, cfg.hp_potion_pct * 100.0f,
        cfg.attack_cooldown_ms, cfg.pickup_cooldown_ms,
        (int)cfg.potions.size());
}

inline void stop() {
    g_running.store(false);
    g_target_id = 0;
    console::info("[BOT] Stopped");
}

} // namespace bot
