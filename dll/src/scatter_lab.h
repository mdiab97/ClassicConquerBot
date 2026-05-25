#pragma once
#include "game.h"
#include "entity_scanner.h"
#include "console.h"
#include "bot.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <fstream>
#include <cmath>
#include <atomic>
#include <string>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════════
// Scatter - AoE skill targeting + casting
// ═══════════════════════════════════════════════════════════════════════

namespace scatter_lab {

static constexpr double PI = 3.14159265358979;

inline double dist(int x1, int y1, int x2, int y2) {
    double dx = x2 - x1, dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

inline double angle_deg(int fx, int fy, int tx, int ty) {
    return std::atan2(static_cast<double>(ty - fy), static_cast<double>(tx - fx)) * 180.0 / PI;
}

inline double normalize_delta(double delta) {
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta;
}

// Check if a monster at (mx, my) would be hit by scatter aimed at (tx, ty)
inline bool predict_hit(int hx, int hy, int tx, int ty, int mx, int my,
                        double range, double half_angle_deg) {
    double d = dist(hx, hy, mx, my);
    if (d > range || d < 0.5) return false;
    double aim = angle_deg(hx, hy, tx, ty);
    double mon = angle_deg(hx, hy, mx, my);
    return std::abs(normalize_delta(mon - aim)) <= half_angle_deg;
}

// ── Monster snapshot ─────────────────────────────────────────────────

// Scatter hit blacklist: monsters predicted to be hit are skipped temporarily
inline std::unordered_map<uint32_t, int64_t> g_scatter_hit_blacklist; // id -> expiry

inline bool is_scatter_blacklisted(uint32_t id) {
    auto it = g_scatter_hit_blacklist.find(id);
    if (it == g_scatter_hit_blacklist.end()) return false;
    if ((int64_t)GetTickCount64() > it->second) {
        g_scatter_hit_blacklist.erase(it);
        return false;
    }
    return true;
}

struct MonsterSnapshot {
    uint32_t id{0};
    int x{0}, y{0};
    bool was_dead{false};
    char name[17]{};
};

inline std::vector<MonsterSnapshot> snapshot_monsters() {
    std::vector<MonsterSnapshot> result;
    auto nearby = bot::fast_scan_nearby();
    for (auto& e : nearby) {
        if (!e.is_monster()) continue;
        if (is_scatter_blacklisted(e.id)) continue; // skip recently hit
        MonsterSnapshot ms;
        ms.id = e.id;
        ms.x = e.x;
        ms.y = e.y;
        ms.was_dead = e.is_dead;
        memcpy(ms.name, e.name, 17);
        result.push_back(ms);
    }
    return result;
}

// ── Targeting: find the best aim direction ───────────────────────────

struct AimResult {
    int target_x{0}, target_y{0};
    int predicted_hits{0};
};

inline AimResult find_best_aim(int hx, int hy,
                               const std::vector<MonsterSnapshot>& monsters,
                               double range, double half_angle_deg) {
    AimResult best;

    std::vector<std::pair<int, int>> candidates;
    for (auto& m : monsters) {
        if (m.was_dead) continue;
        if (dist(hx, hy, m.x, m.y) > range) continue;
        candidates.push_back({m.x, m.y});
    }
    if (candidates.empty()) return best;

    // Centroid
    double cx = 0, cy = 0;
    for (auto& [x, y] : candidates) { cx += x; cy += y; }
    cx /= candidates.size(); cy /= candidates.size();
    candidates.push_back({static_cast<int>(std::round(cx)), static_cast<int>(std::round(cy))});

    // Midpoints
    size_t n = std::min(candidates.size(), static_cast<size_t>(8));
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            candidates.push_back({(candidates[i].first + candidates[j].first) / 2,
                                  (candidates[i].second + candidates[j].second) / 2});

    for (auto& [tx, ty] : candidates) {
        if (tx == hx && ty == hy) continue;
        int hits = 0;
        for (auto& m : monsters) {
            if (!m.was_dead && predict_hit(hx, hy, tx, ty, m.x, m.y, range, half_angle_deg))
                hits++;
        }
        if (hits > best.predicted_hits) {
            best.target_x = tx;
            best.target_y = ty;
            best.predicted_hits = hits;
        }
    }
    return best;
}

// ── Strategic positioning ────────────────────────────────────────────

struct PositionResult {
    int move_x{0}, move_y{0};
    int aim_x{0}, aim_y{0};
    int predicted_hits{0};
};

inline PositionResult find_best_position(int hx, int hy,
                                         const std::vector<MonsterSnapshot>& monsters,
                                         double range, double half_angle_deg,
                                         int max_jump_dist) {
    PositionResult best;

    std::vector<std::pair<int, int>> alive;
    for (auto& m : monsters)
        if (!m.was_dead) alive.push_back({m.x, m.y});
    if (alive.empty()) return best;

    double cx = 0, cy = 0;
    for (auto& [x, y] : alive) { cx += x; cy += y; }
    cx /= alive.size(); cy /= alive.size();
    int centroid_x = static_cast<int>(std::round(cx));
    int centroid_y = static_cast<int>(std::round(cy));

    std::vector<std::pair<int, int>> positions;
    positions.push_back({hx, hy});
    for (int step = 0; step < 8; step++) {
        double a = step * PI / 4.0;
        for (int r = 2; r <= static_cast<int>(range); r += 3) {
            positions.push_back({centroid_x + static_cast<int>(std::round(std::cos(a) * r)),
                                 centroid_y + static_cast<int>(std::round(std::sin(a) * r))});
        }
    }
    for (auto& [mx, my] : alive) {
        positions.push_back({mx, my});
        positions.push_back({mx + 3, my}); positions.push_back({mx - 3, my});
        positions.push_back({mx, my + 3}); positions.push_back({mx, my - 3});
    }

    for (auto& [px, py] : positions) {
        if (dist(hx, hy, px, py) > max_jump_dist) continue;
        auto aim = find_best_aim(px, py, monsters, range, half_angle_deg);
        if (aim.predicted_hits > best.predicted_hits) {
            best.move_x = px; best.move_y = py;
            best.aim_x = aim.target_x; best.aim_y = aim.target_y;
            best.predicted_hits = aim.predicted_hits;
        }
    }
    return best;
}

// ── Packet sending ───────────────────────────────────────────────────

inline bool send_scatter(uintptr_t hero, int tx, int ty, uint32_t magic_id) {
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
    pb[pos++] = 0x10; pos += write_varint(pb + pos, 0);
    pb[pos++] = 0x18; pos += write_varint(pb + pos, static_cast<uint32_t>(tx));
    pb[pos++] = 0x20; pos += write_varint(pb + pos, static_cast<uint32_t>(ty));
    pb[pos++] = 0x30; pos += write_varint(pb + pos, 21);
    pb[pos++] = 0x38; pos += write_varint(pb + pos, 0);
    pb[pos++] = 0x40; pos += write_varint(pb + pos, magic_id);
    pb[pos++] = 0x48; pos += write_varint(pb + pos, 0);

    uint16_t total_size = static_cast<uint16_t>(4 + pos);
    uint16_t msg_type = 1022;
    uint8_t packet[256];
    memcpy(packet, &total_size, 2);
    memcpy(packet + 2, &msg_type, 2);
    memcpy(packet + 4, pb, pos);

    uintptr_t base = game::get_base();
    using GetMgr_t = uintptr_t(__fastcall*)();
    auto get_mgr = reinterpret_cast<GetMgr_t>(base + game::FN_GET_SEND_MGR);
    uintptr_t mgr = get_mgr();
    if (!mgr) return false;
    uintptr_t net_buf = 0;
    game::safe_read(mgr + 32, net_buf);
    if (!net_buf) return false;

    using RawSend_t = int64_t(__fastcall*)(uintptr_t, uint16_t*, int64_t);
    auto raw_send = reinterpret_cast<RawSend_t>(base + 0x1DBA40);
    raw_send(net_buf, reinterpret_cast<uint16_t*>(packet), static_cast<int64_t>(total_size));
    return true;
}

// ── Bot scatter tick ─────────────────────────────────────────────────

inline bool bot_scatter_tick(uintptr_t hero, int hx, int hy, int64_t now) {
    if (!bot::g_config.use_scatter || !bot::g_has_scatter_magic) return false;
    if (now - bot::g_last_scatter_time < bot::g_config.scatter_cooldown_ms) return false;

    auto monsters = snapshot_monsters();
    int alive_in_range = 0;
    for (auto& m : monsters)
        if (!m.was_dead && dist(hx, hy, m.x, m.y) <= bot::g_config.scatter_range)
            alive_in_range++;
    if (alive_in_range < bot::g_config.scatter_min_targets) return false;

    auto aim = find_best_aim(hx, hy, monsters,
        bot::g_config.scatter_range, bot::g_config.scatter_half_angle);
    if (aim.predicted_hits < bot::g_config.scatter_min_targets) return false;

    if (!send_scatter(hero, aim.target_x, aim.target_y, bot::g_config.scatter_magic_id))
        return false;

    // Blacklist predicted hits so bot keeps moving forward
    if (bot::g_config.scatter_predict_hits) {
        int64_t expiry = now + 3000;
        for (auto& m : monsters) {
            if (!m.was_dead && predict_hit(hx, hy, aim.target_x, aim.target_y,
                    m.x, m.y, bot::g_config.scatter_range, bot::g_config.scatter_half_angle)) {
                g_scatter_hit_blacklist[m.id] = expiry;
            }
        }
    }

    bot::g_last_scatter_time = now;
    return true;
}

// Manual test from GUI
inline bool begin_test(int tx, int ty) {
    uintptr_t hero = game::get_hero();
    if (!hero) return false;
    return send_scatter(hero, tx, ty, bot::g_config.scatter_magic_id);
}

// No-op tick (evaluation removed)
inline void tick() {}

inline void init(const std::string&) {}

} // namespace scatter_lab
