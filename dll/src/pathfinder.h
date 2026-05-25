#pragma once
#include "game.h"
#include "console.h"
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <thread>

namespace pathfinder {

// GameMap offsets (from IDA render code analysis)
// GameMap singleton at base+0x4E02E0
// GameMap+0x30 (48) = map width (int)
// GameMap+0x34 (52) = map height (int)
// GameMap+0x58 (88) = CellInfo* array pointer
// CellInfo size = 24 bytes (from render code: cellArray + 24 * index)
// CellInfo layout: LayerInfo at +0 (usTerrain:u16, usMask:u16, sAltitude:i16, padding, pLayer:ptr)
// Mask == 1 means blocked

constexpr int GAMEMAP_WIDTH_OFF  = 0x30;
constexpr int GAMEMAP_HEIGHT_OFF = 0x34;
constexpr int GAMEMAP_CELLS_OFF  = 0x58;
constexpr int CELLINFO_SIZE      = 24;
constexpr int MAX_JUMP_DIST      = 11;
constexpr int MAX_ALT_DIFF       = 200;

// ═══════════════════════════════════════════════════════════════════════
// CACHED MAP GRID — read once, pathfind fast
// ═══════════════════════════════════════════════════════════════════════

struct CachedCell { uint16_t mask; int16_t alt; };
inline std::vector<CachedCell> g_grid;
inline int g_grid_w{0}, g_grid_h{0};
inline int64_t g_grid_time{0}; // when grid was cached

inline uintptr_t get_gamemap() { return game::get_base() + 0x4E02E0; }

inline bool get_map_size(int& width, int& height) {
    uintptr_t gm = get_gamemap();
    game::safe_read<int>(gm + GAMEMAP_WIDTH_OFF, width);
    game::safe_read<int>(gm + GAMEMAP_HEIGHT_OFF, height);
    return width > 0 && height > 0 && width < 2000 && height < 2000;
}

// Cache entire cell grid into local memory (called once per map or periodically)
inline void cache_grid() {
    auto t0 = GetTickCount64();
    uintptr_t gm = get_gamemap();
    int w = 0, h = 0;
    game::safe_read<int>(gm + GAMEMAP_WIDTH_OFF, w);
    game::safe_read<int>(gm + GAMEMAP_HEIGHT_OFF, h);
    if (w <= 0 || h <= 0 || w > 2000 || h > 2000) {
        console::warn("[GRID] Invalid map size %dx%d", w, h);
        return;
    }

    uintptr_t cells = 0;
    game::safe_read(gm + GAMEMAP_CELLS_OFF, cells);
    if (!cells || !game::is_valid_ptr(cells)) {
        console::warn("[GRID] Invalid cells ptr %p", (void*)cells);
        return;
    }

    int64_t total_cells = (int64_t)w * h;
    int64_t total_bytes = total_cells * CELLINFO_SIZE;

    if (!game::is_valid_ptr(cells + total_bytes - 1)) {
        console::warn("[GRID] Cell array end %p not valid", (void*)(cells + total_bytes - 1));
        return;
    }

    std::vector<uint8_t> raw(total_bytes);
    memcpy(raw.data(), reinterpret_cast<void*>(cells), total_bytes);

    g_grid.resize(total_cells);
    g_grid_w = w;
    g_grid_h = h;

    int walkable = 0, blocked = 0, layered = 0;
    for (int64_t i = 0; i < total_cells; i++) {
        uint8_t* c = raw.data() + i * CELLINFO_SIZE;
        // CellInfo layout: LayerInfo { uint16 terrain(+0), uint16 mask(+2), int16 alt(+4), pad(+6), LayerInfo* pLayer(+8) }
        // Follow pLayer chain to get the TOP layer's mask and altitude
        uint16_t mask = *reinterpret_cast<uint16_t*>(c + 2);
        int16_t alt = *reinterpret_cast<int16_t*>(c + 4);
        uintptr_t next_layer = *reinterpret_cast<uintptr_t*>(c + 8);

        // Walk layer chain (in live game memory, not in our raw copy)
        int chain = 0;
        while (next_layer && game::is_valid_ptr(next_layer) && chain++ < 10) {
            layered++;
            uint16_t layer_mask = 0;
            int16_t layer_alt = 0;
            uintptr_t layer_next = 0;
            game::safe_read<uint16_t>(next_layer + 2, layer_mask);
            game::safe_read<int16_t>(next_layer + 4, layer_alt);
            game::safe_read<uintptr_t>(next_layer + 8, layer_next);
            mask = layer_mask;
            alt = layer_alt;
            next_layer = layer_next;
        }

        g_grid[i].mask = mask;
        g_grid[i].alt = alt;
        if (mask == 1) blocked++; else walkable++;
    }
    console::info("[GRID] %d layered cells resolved", layered);
    g_grid_time = GetTickCount64();

    // Debug: dump a few mask values around hero position
    int hx = 0, hy = 0;
    game::get_pos(hx, hy);
    if (hx > 0 && hy > 0 && hx < w && hy < h) {
        int hi = hy * w + hx;
        console::info("[GRID] Hero at (%d,%d) mask=%d alt=%d", hx, hy, g_grid[hi].mask, g_grid[hi].alt);
        // Sample raw bytes from hero's cell
        uint8_t* raw_cell = raw.data() + (int64_t)hi * CELLINFO_SIZE;
        console::info("[GRID] Raw cell bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
            raw_cell[0], raw_cell[1], raw_cell[2], raw_cell[3],
            raw_cell[4], raw_cell[5], raw_cell[6], raw_cell[7]);
        // Also check neighbors
        int masks[9] = {};
        int idx = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                masks[idx++] = g_grid[(hy+dy)*w+(hx+dx)].mask;
        console::info("[GRID] 3x3 masks around hero: %d %d %d / %d %d %d / %d %d %d",
            masks[0],masks[1],masks[2], masks[3],masks[4],masks[5], masks[6],masks[7],masks[8]);
    }

    console::info("[GRID] Cached %dx%d in %lldms (%d walkable, %d blocked)",
        w, h, GetTickCount64() - t0, walkable, blocked);
}

// Live cell pointer lookup
inline uintptr_t get_cell_ptr(int x, int y) {
    uintptr_t gm = get_gamemap();
    int w = 0, h = 0;
    game::safe_read<int>(gm + GAMEMAP_WIDTH_OFF, w);
    game::safe_read<int>(gm + GAMEMAP_HEIGHT_OFF, h);
    if (x < 0 || x >= w || y < 0 || y >= h) return 0;
    uintptr_t cells = 0;
    game::safe_read(gm + GAMEMAP_CELLS_OFF, cells);
    if (!cells) return 0;
    return cells + CELLINFO_SIZE * (x + (int64_t)y * w);
}

// Live single-cell reads (used when grid not cached or for real-time checks)
inline bool is_blocked_live(int x, int y) {
    uintptr_t c = get_cell_ptr(x, y);
    if (!c) return true;
    uint16_t mask = 0;
    game::safe_read<uint16_t>(c + 2, mask);
    return mask == 1;
}
inline int get_altitude_live(int x, int y) {
    uintptr_t c = get_cell_ptr(x, y);
    if (!c) return 0;
    int16_t alt = 0;
    game::safe_read<int16_t>(c + 4, alt);
    return alt;
}

// Fast grid lookups (falls back to live reads when grid not cached)
inline bool grid_valid(int x, int y) {
    return x >= 0 && y >= 0 && x < g_grid_w && y < g_grid_h;
}
inline bool is_blocked(int x, int y) {
    if (g_grid.empty()) return is_blocked_live(x, y);
    if (!grid_valid(x, y)) return true;
    return g_grid[y * g_grid_w + x].mask == 1;
}
inline int get_altitude(int x, int y) {
    if (g_grid.empty()) return get_altitude_live(x, y);
    if (!grid_valid(x, y)) return 0;
    return g_grid[y * g_grid_w + x].alt;
}
inline int get_mask(int x, int y) {
    if (g_grid.empty()) {
        uintptr_t c = get_cell_ptr(x, y);
        if (!c) return 1;
        uint16_t m = 0; game::safe_read<uint16_t>(c + 2, m); return m;
    }
    if (!grid_valid(x, y)) return 1;
    return g_grid[y * g_grid_w + x].mask;
}

inline bool can_jump_to(int fx, int fy, int tx, int ty) {
    if (fx == tx && fy == ty) return false;
    int dx = tx - fx, dy = ty - fy;
    if (dx * dx + dy * dy > MAX_JUMP_DIST * MAX_JUMP_DIST) return false;

    // Use cached grid if available, otherwise live reads
    if (!g_grid.empty()) {
        if (is_blocked(tx, ty)) return false;
        int alt_diff = get_altitude(tx, ty) - get_altitude(fx, fy);
        if (alt_diff > MAX_ALT_DIFF) return false;
    } else {
        if (is_blocked_live(tx, ty)) return false;
        int alt_diff = get_altitude_live(tx, ty) - get_altitude_live(fx, fy);
        if (alt_diff > MAX_ALT_DIFF) return false;
    }
    return true;
}

inline bool is_occupied(int x, int y) { return false; }

// ═══════════════════════════════════════════════════════════════════════
// A* PATHFINDER — cell-level on cached grid, then compress to jump waypoints
// ═══════════════════════════════════════════════════════════════════════

inline int encode_pos(int x, int y) { return (x & 0xFFFF) | (y << 16); }

// Invalidate grid cache (call on map change)
inline void invalidate_grid() {
    g_grid.clear();
    g_grid_w = 0;
    g_grid_h = 0;
    g_grid_time = 0;
    console::info("[GRID] Invalidated");
}

// Full cell-level A* on cached grid. All lookups are local array reads = fast.
inline std::vector<game::CMyPos> astar_cell(int sx, int sy, int ex, int ey) {
    int w = g_grid_w, h = g_grid_h;
    if (w <= 0 || h <= 0) return {};
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
        console::warn("[ASTAR] Start (%d,%d) out of bounds (%dx%d)", sx, sy, w, h);
        return {};
    }
    if (ex < 0 || ey < 0 || ex >= w || ey >= h) {
        console::warn("[ASTAR] End (%d,%d) out of bounds (%dx%d)", ex, ey, w, h);
        return {};
    }

    int total = w * h;
    // Use a single allocation for g_cost + parent (8 bytes per cell)
    // For 1000x1000 = 8MB, acceptable for a one-off pathfind
    std::vector<int> g_cost(total, INT_MAX);
    std::vector<int> parent(total, -1);
    std::vector<bool> closed(total, false);

    struct Open { int f, idx; bool operator>(const Open& o) const { return f > o.f; } };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> pq;

    static const int dx8[] = {1,-1,0,0, 1,1,-1,-1};
    static const int dy8[] = {0,0,1,-1, 1,-1,1,-1};
    static const int cost8[] = {10,10,10,10, 14,14,14,14};

    auto heur = [&](int x, int y) -> int {
        int dx = std::abs(ex - x), dy = std::abs(ey - y);
        return 10 * std::max(dx, dy) + 4 * std::min(dx, dy); // octile
    };

    int si = sy * w + sx, ei = ey * w + ex;

    // Debug: check start/end cells
    console::info("[ASTAR] grid %dx%d, start(%d,%d) mask=%d alt=%d, end(%d,%d) mask=%d alt=%d",
        w, h, sx, sy, g_grid[si].mask, g_grid[si].alt, ex, ey, g_grid[ei].mask, g_grid[ei].alt);

    g_cost[si] = 0;
    pq.push({heur(sx, sy), si});

    int iters = 0;
    while (!pq.empty()) {
        auto [cf, ci] = pq.top(); pq.pop();
        if (ci == ei) break;
        if (closed[ci]) continue;
        closed[ci] = true;
        iters++;

        // Safety: if we've explored too many nodes, bail
        if (iters > 500000) {
            console::warn("[ASTAR] Exceeded 500k iterations, aborting");
            break;
        }

        int cx = ci % w, cy = ci / w;

        for (int d = 0; d < 8; d++) {
            int nx = cx + dx8[d], ny = cy + dy8[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            int ni = ny * w + nx;
            if (closed[ni]) continue;
            if (g_grid[ni].mask == 1) continue;

            int ng = g_cost[ci] + cost8[d];
            if (ng >= g_cost[ni]) continue;
            g_cost[ni] = ng;
            parent[ni] = ci;
            pq.push({ng + heur(nx, ny), ni});
        }
    }

    console::info("[ASTAR] Done: %d iters, path %s", iters, g_cost[ei] != INT_MAX ? "FOUND" : "NOT FOUND");

    if (g_cost[ei] == INT_MAX) return {};

    std::vector<game::CMyPos> path;
    for (int ci = ei; ci != si && ci >= 0; ci = parent[ci])
        path.push_back({ci % w, ci / w});
    std::reverse(path.begin(), path.end());
    return path;
}

// Compress cell path to jump waypoints: skip to farthest reachable within jump range
inline std::vector<game::CMyPos> compress_to_jumps(int sx, int sy, const std::vector<game::CMyPos>& cell_path) {
    if (cell_path.empty()) return {};

    // Prepend start position
    std::vector<game::CMyPos> full;
    full.reserve(cell_path.size() + 1);
    full.push_back({sx, sy});
    for (auto& p : cell_path) full.push_back(p);

    std::vector<game::CMyPos> jumps;
    int cur = 0;
    while (cur < (int)full.size() - 1) {
        int best = cur + 1;
        for (int ahead = (int)full.size() - 1; ahead > cur; ahead--) {
            if (can_jump_to(full[cur].x, full[cur].y, full[ahead].x, full[ahead].y)) {
                best = ahead;
                break;
            }
        }
        jumps.push_back(full[best]);
        cur = best;
    }
    return jumps;
}

// Main entry point
inline std::vector<game::CMyPos> find_path(int sx, int sy, int ex, int ey) {
    auto t0 = GetTickCount64();
    console::info("[PATH] find_path(%d,%d -> %d,%d) grid=%s age=%lldms",
        sx, sy, ex, ey,
        g_grid.empty() ? "EMPTY" : "CACHED",
        g_grid.empty() ? 0 : (int64_t)(GetTickCount64() - g_grid_time));

    if (g_grid.empty() || (GetTickCount64() - g_grid_time > 30000)) {
        console::info("[PATH] Caching grid...");
        cache_grid();
    }
    if (g_grid.empty()) {
        console::warn("[PATH] Grid cache failed, no path");
        return {};
    }

    // Fix blocked start (hero is there so it's obviously reachable)
    if (is_blocked(sx, sy)) {
        console::warn("[PATH] Start (%d,%d) is blocked (mask=%d), adjusting...", sx, sy, get_mask(sx, sy));
        bool found = false;
        for (int r = 1; r <= 5 && !found; r++)
            for (int dy = -r; dy <= r && !found; dy++)
                for (int dx = -r; dx <= r && !found; dx++)
                    if (!is_blocked(sx + dx, sy + dy)) { sx += dx; sy += dy; found = true; }
        if (!found) {
            console::warn("[PATH] No walkable cell near start");
            return {};
        }
        console::info("[PATH] Adjusted start to (%d,%d)", sx, sy);
    }

    // Fix blocked destination
    if (is_blocked(ex, ey)) {
        console::info("[PATH] Dest (%d,%d) blocked (mask=%d), searching nearby...", ex, ey, get_mask(ex, ey));
        bool found = false;
        for (int r = 1; r <= 10 && !found; r++)
            for (int dy = -r; dy <= r && !found; dy++)
                for (int dx = -r; dx <= r && !found; dx++)
                    if (!is_blocked(ex + dx, ey + dy)) { ex += dx; ey += dy; found = true; }
        if (!found) {
            console::warn("[PATH] No walkable cell near destination");
            return {};
        }
        console::info("[PATH] Adjusted dest to (%d,%d)", ex, ey);
    }

    console::info("[PATH] Running A*...");
    auto cells = astar_cell(sx, sy, ex, ey);
    if (cells.empty()) {
        console::warn("[PATH] A* found no path");
        return {};
    }

    auto jumps = compress_to_jumps(sx, sy, cells);
    console::info("[PATH] Done in %lldms: %zu cells -> %zu jumps",
        GetTickCount64() - t0, cells.size(), jumps.size());
    return jumps;
}

/*  OLD CODE REMOVED — replaced by proper astar_cell above
inline std::vector<game::CMyPos> astar_coarse(int sx, int sy, int ex, int ey) {
    constexpr int STEP = 5; // downsample factor
    int w = g_grid_w, h = g_grid_h;
    int cw = (w + STEP - 1) / STEP;
    int ch = (h + STEP - 1) / STEP;

    // Check if a coarse cell is walkable (any walkable cell in the STEP×STEP block)
    auto coarse_blocked = [&](int cx, int cy) -> bool {
        int bx = cx * STEP, by = cy * STEP;
        for (int dy = 0; dy < STEP && by + dy < h; dy++)
            for (int dx = 0; dx < STEP && bx + dx < w; dx++)
                if (g_grid[(by + dy) * w + (bx + dx)].mask != 1) return false;
        return true; // entire block is blocked
    };

    // Coarse coords
    int csx = sx / STEP, csy = sy / STEP;
    int cex = ex / STEP, cey = ey / STEP;
    if (csx == cex && csy == cey) {
        // Same coarse cell — just return destination directly
        return {{ex, ey}};
    }

    std::vector<int> g_cost(cw * ch, INT_MAX);
    std::vector<int> parent(cw * ch, -1);

    struct Open { int f; int idx; bool operator>(const Open& o) const { return f > o.f; } };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> pq;

    auto cidx = [cw](int x, int y) { return y * cw + x; };
    auto heur = [](int x, int y, int gx, int gy) -> int {
        return std::max(std::abs(gx - x), std::abs(gy - y));
    };

    int si = cidx(csx, csy), ei = cidx(cex, cey);
    g_cost[si] = 0;
    pq.push({heur(csx, csy, cex, cey), si});

    static const int dx8[] = {1,-1,0,0,1,1,-1,-1};
    static const int dy8[] = {0,0,1,-1,1,-1,1,-1};

    int iters = 0;
    while (!pq.empty() && iters++ < 20000) {
        auto [cf, ci] = pq.top(); pq.pop();
        if (ci == ei) break;

        int cx = ci % cw, cy = ci / cw;
        int cg = g_cost[ci];
        if (cg + heur(cx, cy, cex, cey) > cf + 1) continue;

        for (int d = 0; d < 8; d++) {
            int nx = cx + dx8[d], ny = cy + dy8[d];
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            if (coarse_blocked(nx, ny)) continue;
            int ni = cidx(nx, ny);
            int ng = cg + 1;
            if (ng >= g_cost[ni]) continue;
            g_cost[ni] = ng;
            parent[ni] = ci;
            pq.push({ng + heur(nx, ny, cex, cey), ni});
        }
    }

    if (g_cost[ei] == INT_MAX) return {}; // no path

    // Reconstruct coarse path -> convert to fine coords (center of each block)
    std::vector<game::CMyPos> path;
    for (int ci = ei; ci >= 0 && ci != si; ci = parent[ci]) {
        int cx = ci % cw, cy = ci / cw;
        // Pick best walkable cell in this block (closest to center)
        int bx = cx * STEP + STEP / 2, by = cy * STEP + STEP / 2;
        if (bx >= w) bx = w - 1;
        if (by >= h) by = h - 1;
        // Find nearest walkable to center
        bool found = false;
        for (int r = 0; r <= STEP && !found; r++) {
            for (int dy = -r; dy <= r && !found; dy++) {
                for (int dx = -r; dx <= r && !found; dx++) {
                    int fx = bx + dx, fy = by + dy;
                    if (fx >= 0 && fy >= 0 && fx < w && fy < h &&
                        g_grid[fy * w + fx].mask != 1) {
                        path.push_back({fx, fy});
                        found = true;
                    }
                }
            }
        }
    }
    std::reverse(path.begin(), path.end());

    // Replace last waypoint with actual destination
    if (!path.empty()) path.back() = {ex, ey};
    return path;
}

// Compress cell path to jump waypoints: skip ahead as far as possible within jump range
inline std::vector<game::CMyPos> compress_to_jumps(const std::vector<game::CMyPos>& cell_path) {
    if (cell_path.empty()) return {};

    std::vector<game::CMyPos> jumps;
    int cur = 0; // index into cell_path we're jumping FROM (start pos)

    while (cur < (int)cell_path.size()) {
        // Find the farthest cell we can jump to directly
        int best = cur; // at minimum, step to next cell
        for (int ahead = (int)cell_path.size() - 1; ahead > cur; ahead--) {
            int dx = cell_path[ahead].x - (cur == 0 ? cell_path[0].x : cell_path[cur].x);
            int dy = cell_path[ahead].y - (cur == 0 ? cell_path[0].y : cell_path[cur].y);
            // Wait — cur==0 means we're at the start (hero pos, not in cell_path)
            // Actually cell_path[0] is the first cell after start
            // Let me just use cell_path positions directly
            if (dx * dx + dy * dy > MAX_JUMP_DIST * MAX_JUMP_DIST) continue;
            if (is_blocked(cell_path[ahead].x, cell_path[ahead].y)) continue;
            // Check altitude
            int alt_from = get_altitude(
                cur == 0 ? cell_path[0].x : cell_path[cur].x,
                cur == 0 ? cell_path[0].y : cell_path[cur].y);
            int alt_to = get_altitude(cell_path[ahead].x, cell_path[ahead].y);
            if (alt_to - alt_from > MAX_ALT_DIFF) continue;
            best = ahead;
            break; // found farthest reachable
        }

        if (best == cur) {
            // Can't skip, just take next cell
            best = cur + 1;
            if (best >= (int)cell_path.size()) break;
        }

        jumps.push_back(cell_path[best]);
        cur = best;

        // If we've reached the last cell, done
        if (cur >= (int)cell_path.size() - 1) break;
    }

    return jumps;
}

// Main pathfinding entry point: A* on cells, then compress to jumps
inline std::vector<game::CMyPos> find_path(int sx, int sy, int ex, int ey) {
    // Ensure grid is cached
    if (g_grid.empty() || (GetTickCount64() - g_grid_time > 30000)) cache_grid();
    if (g_grid.empty()) return {};

    // Fix blocked destination
    if (is_blocked(ex, ey)) {
        bool found = false;
        for (int r = 1; r <= 5 && !found; r++)
            for (int dy = -r; dy <= r && !found; dy++)
                for (int dx = -r; dx <= r && !found; dx++)
                    if (!is_blocked(ex + dx, ey + dy)) { ex += dx; ey += dy; found = true; }
        if (!found) return {};
    }

    // Coarse A* (5x downsampled grid — fast even on 1000x1000 maps)
    auto coarse = astar_coarse(sx, sy, ex, ey);
    if (coarse.empty()) return {};

    // Build full waypoint list with start pos, then compress to jump-sized hops
    std::vector<game::CMyPos> full;
    full.push_back({sx, sy});
    for (auto& p : coarse) full.push_back(p);

    std::vector<game::CMyPos> jumps;
    int cur = 0;
    while (cur < (int)full.size() - 1) {
        int best = cur + 1;
        // Scan backwards from end to find farthest jumpable waypoint
        for (int ahead = (int)full.size() - 1; ahead > cur; ahead--) {
            if (can_jump_to(full[cur].x, full[cur].y, full[ahead].x, full[ahead].y)) {
                best = ahead;
                break;
            }
        }
        jumps.push_back(full[best]);
        cur = best;
    }

    return jumps;
}
END OLD CODE REMOVED */

// Path following state
inline std::vector<game::CMyPos> g_current_path;
inline int g_path_index = 0;
inline std::atomic<bool> g_following{false};
inline int g_dest_x = 0, g_dest_y = 0;

// Start pathfinding to destination
inline void start_pathfind(int dest_x, int dest_y) {
    int hx = 0, hy = 0;
    game::get_pos(hx, hy);

    console::info("[PATH] Finding path from %d,%d to %d,%d", hx, hy, dest_x, dest_y);

    auto path = find_path(hx, hy, dest_x, dest_y);

    if (path.empty()) {
        console::warn("[PATH] No path found!");
        return;
    }

    console::ok("[PATH] Found path with %d jumps", (int)path.size());
    g_current_path = path;
    g_path_index = 0;
    g_dest_x = dest_x;
    g_dest_y = dest_y;
    g_following.store(true);
}

inline bool is_pathfinding() { return g_following.load(); }

// Stop following path
inline void stop_pathfind() {
    g_following.store(false);
    g_current_path.clear();
    g_path_index = 0;
}

// Call every tick to follow path
inline bool g_jump_pending = false;

inline void tick() {
    if (!g_following.load()) return;

    if (g_path_index >= static_cast<int>(g_current_path.size())) {
        console::ok("[PATH] Destination reached!");
        stop_pathfind();
        return;
    }

    // Check if hero is busy using the game's actual action state at hero+0x118
    // IsMoving action IDs: 110,115,120,121,125,126,130,131,132,140,320,470,510
    uintptr_t hero = game::get_hero();
    if (!hero) return;
    int action = 0;
    game::safe_read<int>(hero + 0x118, action);
    // Skip if hero is in a movement/action animation
    switch (action) {
        case 110: case 115: case 120: case 121: case 125: case 126:
        case 130: case 131: case 132: case 140: case 320: case 470: case 510:
            return;
    }

    int hx = 0, hy = 0;
    game::get_pos(hx, hy);

    auto& target = g_current_path[g_path_index];

    // Check if we reached the current waypoint
    int dx = target.x - hx, dy = target.y - hy;
    double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
    if (dist <= 2) {
        g_path_index++;
        return;
    }

    // Out of range - recalculate
    if (dist > MAX_JUMP_DIST) {
        console::warn("[PATH] Jump out of range, recalculating...");
        auto new_path = find_path(hx, hy, g_dest_x, g_dest_y);
        if (new_path.empty()) {
            console::error("[PATH] No path found, stopping");
            stop_pathfind();
            return;
        }
        g_current_path = new_path;
        g_path_index = 0;
        return;
    }

    // Humanize: walk short distances instead of jumping
    // - Final waypoint within 5 cells: always walk
    // - Random ~20% chance to walk if distance <= 4 cells
    bool is_last = (g_path_index == (int)g_current_path.size() - 1);
    bool short_dist = (dist <= 5.0);
    bool should_walk = (is_last && short_dist) || (short_dist && dist <= 4.0 && (rand() % 5) == 0);

    if (should_walk) {
        console::info("[PATH] Walk %d/%d -> %d,%d (%.0f cells)",
            g_path_index + 1, (int)g_current_path.size(), target.x, target.y, dist);
        game::queue_run(target.x, target.y);
    } else {
        console::info("[PATH] Jump %d/%d -> %d,%d",
            g_path_index + 1, (int)g_current_path.size(), target.x, target.y);
        uint8_t jcmd[0x108]{};
        *reinterpret_cast<int32_t*>(jcmd + 0x00) = game::CMD_JUMP;
        *reinterpret_cast<int32_t*>(jcmd + 0x0C) = target.x;
        *reinterpret_cast<int32_t*>(jcmd + 0x10) = target.y;
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(hero);
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtable + 0x1D8);
        reinterpret_cast<void(__fastcall*)(uintptr_t, void*)>(fn)(hero, jcmd);
    }
}

// Debug: dump cell info around hero
inline void debug_cells() {
    int hx = 0, hy = 0;
    game::get_pos(hx, hy);
    int w = 0, h = 0;
    get_map_size(w, h);
    console::info("[MAP] Size: %dx%d, Hero at: %d,%d", w, h, hx, hy);

    // Dump raw cell pointer and first cell bytes
    uintptr_t gm = get_gamemap();
    uintptr_t cells = 0;
    game::safe_read(gm + GAMEMAP_CELLS_OFF, cells);
    console::info("[MAP] GameMap=0x%llX CellPtr=0x%llX (off=0x%X) %s",
        gm, cells, GAMEMAP_CELLS_OFF, (cells && game::is_valid_ptr(cells)) ? "VALID" : "INVALID");

    // If cells invalid, scan for the right offset
    if (!cells || !game::is_valid_ptr(cells)) {
        console::info("[MAP] Scanning GameMap for cell array pointer...");
        for (int off = 0; off < 200; off += 8) {
            uintptr_t val = 0;
            game::safe_read(gm + off, val);
            if (val && game::is_valid_ptr(val) && val > 0x1000000) {
                // Try reading as cell array - check if index 0 has reasonable data
                uint16_t test_mask = 0;
                game::safe_read<uint16_t>(val + 2, test_mask);
                if (test_mask <= 1) {
                    console::info("  gm+0x%02X = 0x%llX (ptr, cell[0].mask=%d)", off, val, test_mask);
                }
            }
        }
    } else {
        // Dump raw bytes of cell at hero position
        int idx = hx + hy * w;
        console::info("[MAP] Cell[%d,%d] index=%d:", hx, hy, idx);
        // Try different cell sizes: 8, 16, 24, 32
        // Dump raw hex of cell at different sizes
        for (int cs : {24}) {
            uintptr_t cell_addr = cells + static_cast<int64_t>(idx) * cs;
            if (!game::is_valid_ptr(cell_addr)) continue;
            console::info("  Cell at 0x%llX (size=%d):", cell_addr, cs);
            char hex[128];
            int hp = 0;
            for (int off = 0; off < cs; off++) {
                uint8_t b = 0;
                game::safe_read<uint8_t>(cell_addr + off, b);
                hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", b);
            }
            console::info("    %s", hex);
        }
        // Also try a blocked cell (check nearby for a wall)
        // Walk in one direction to find a cell with mask=1
        console::info("  Scanning for blocked cells nearby...");
        for (int scan = 1; scan < 30; scan++) {
            for (int dir = 0; dir < 4; dir++) {
                int sx = hx + (dir == 0 ? scan : dir == 1 ? -scan : 0);
                int sy = hy + (dir == 2 ? scan : dir == 3 ? -scan : 0);
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                int sidx = sx + sy * w;
                uintptr_t scell = cells + static_cast<int64_t>(sidx) * 24;
                if (!game::is_valid_ptr(scell)) continue;
                // Read first 24 bytes
                bool has_data = false;
                for (int off = 0; off < 24; off += 2) {
                    uint16_t v = 0;
                    game::safe_read<uint16_t>(scell + off, v);
                    if (v != 0) { has_data = true; break; }
                }
                if (has_data) {
                    char hex2[128];
                    int hp2 = 0;
                    for (int off = 0; off < 24; off++) {
                        uint8_t b = 0;
                        game::safe_read<uint8_t>(scell + off, b);
                        hp2 += snprintf(hex2 + hp2, sizeof(hex2) - hp2, "%02X ", b);
                    }
                    console::info("  Cell[%d,%d]: %s", sx, sy, hex2);
                    if (scan > 5) break; // just a few
                }
            }
            if (scan > 10) break;
        }
    }

    // Dump 5x5 grid around hero
    console::info("[MAP] Cell grid (mask/alt):");
    for (int dy = -2; dy <= 2; dy++) {
        char line[256];
        int pos = 0;
        for (int dx = -2; dx <= 2; dx++) {
            int cx = hx + dx, cy = hy + dy;
            int mask = get_mask(cx, cy);
            int alt = get_altitude(cx, cy);
            bool occ = is_occupied(cx, cy);
            char marker = (dx == 0 && dy == 0) ? '@' : (mask == 1 ? '#' : (occ ? 'O' : '.'));
            pos += snprintf(line + pos, sizeof(line) - pos, "%c(%d/%d) ", marker, mask, alt);
        }
        console::info("  %s", line);
    }
}

} // namespace pathfinder
