# ClassicConquerBot v2.0

Bot and toolkit for Classic Conquer Online private servers. Two-piece architecture: a GUI controller that manages accounts, displays game state, and a DLL that gets injected into the game client to read memory and execute actions.

## What it does

- **Multi-account management** — launch and control multiple game clients from one window
- **Auto hunting** — melee, ranged, scatter (AoE), and magic combat modes with configurable behavior
- **Pathfinding** — A* grid pathfinding using the game's .dmap terrain data, with jump validation
- **Auto potions** — HP and MP potion usage at configurable thresholds
- **Looting** — configurable pickup filters (by type, quality, plus level), potion restocking
- **XP skill usage** — auto-cast cyclone/superman/fly when XP bar fills
- **Cross-map travel** — automated navigation between maps using portal/gateway database
- **Portal discovery** — walk to portals and record where they lead (builds Gateways.json)
- **HWID spoofing** — IAT hooks for username, volume serial, computer name, system info
- **Player safety** — detects nearby players/GMs, can flee, teleport to safe spot, or disconnect
- **Network proxy** — MITM proxy between client and server for packet capture and inspection
- **Warehouse deposit** — auto-deposit inventory to warehouse NPC
- **VIP teleport** — one-click city teleports with cooldown tracking
- **NPC interaction** — activate NPCs, read dialog options, send responses
- **Minimap overlay** — renders the game map with entity positions in the GUI
- **Quest data recorder** — tag NPCs, items, monsters, and dialog flows for quest automation research

## Project layout

```
repo/
├── CMakeLists.txt          # root build, pulls dependencies via CPM
├── cmake/
│   └── CPM.cmake           # CPM package manager bootstrap
├── common/                 # shared networking library (TCP client/server, message protocol)
│   ├── CMakeLists.txt
│   ├── include/common/net/
│   │   ├── message.h       # binary message format with << >> operators
│   │   ├── tcp_client.h    # async TCP client (asio)
│   │   └── tcp_server.h    # async TCP server (asio)
│   └── src/
│       ├── tcp_client.cpp
│       └── tcp_server.cpp
├── dll/                    # injected DLL (d3d11_config.dll)
│   ├── CMakeLists.txt
│   └── src/
│       ├── dllmain.cpp     # entry point, message dispatch, state loop
│       ├── game.h          # memory offsets, field accessors, VMT hook, action queue
│       ├── bot.h           # hunting logic (melee/ranged/scatter/magic), exploration
│       ├── pathfinder.h    # A* pathfinding on dmap grid, jump validation
│       ├── entity_scanner.h # reads PlayerSet deque for nearby entities + ground items
│       ├── field_scanner.h # memory scanner (search int/string across game memory)
│       ├── scatter_lab.h   # AoE scatter skill positioning and hit prediction
│       ├── cross_map.h     # multi-map travel using gateway database
│       ├── auto_deposit.h  # warehouse auto-deposit state machine
│       ├── login.h         # auto-login (writes credentials to game UI fields)
│       ├── player_safety.h # player/GM detection and response
│       ├── net_hook.h      # network send/recv hooks (MinHook)
│       ├── overlay.h       # in-game ImGui overlay (unused currently)
│       ├── console.h       # colored console logging
│       ├── hwid_spoof.cpp  # IAT patching for HWID spoofing
│       ├── hwid_spoof.h
│       └── proxy/
│           ├── version_proxy.cpp  # version.dll proxy forwarding
│           └── version_proxy.h
└── gui/                    # controller GUI (CQGraphicsConfig.exe)
    ├── CMakeLists.txt
    └── src/
        ├── main.cpp        # Win32 window + message loop
        ├── app.cpp         # ImGui rendering, network callbacks, all UI panels
        ├── app.h           # App class, HeroState, UI state
        ├── injector.h      # manual map DLL injector (x64)
        ├── proto_lite.h    # lightweight protobuf decoder for packet inspection
        ├── proxy_connection.h  # per-account network proxy (login + game server)
        ├── stb_image.h     # stb_image (single header image loading)
        └── wdf.h           # WDF archive reader (game asset files)
```

## Building

Requirements:
- Windows 10/11
- Visual Studio 2022 (MSVC v143+)
- CMake 3.24+

Dependencies are pulled automatically by CPM (no manual setup needed):
- [asio](https://github.com/chriskohlhoff/asio) 1.30.2 — async networking
- [imgui](https://github.com/ocornut/imgui) 1.91.6 — GUI
- [minhook](https://github.com/TsudaKageyu/minhook) 1.3.3 — API hooking
- [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 — JSON parsing

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output goes to `build/bin/Release/`:
- `CQGraphicsConfig.exe` — the GUI (run as admin)
- `d3d11_config.dll` — the DLL (injected by the GUI)

## Usage

1. **Set your client path** — point the GUI to your Conquer Online client folder (the one containing `bin/64/ImConquer.exe`).

2. **Add accounts** — enter username, password, and server index in the accounts panel.

3. **Launch** — click Launch to start the game client. The GUI injects the DLL automatically via manual mapping. The DLL opens a console window titled "D3D11 Config" showing its status.

4. **Wait for connection** — the DLL connects back to the GUI over TCP (port 1411). Once connected, the hero panel shows your character's live stats.

5. **Bot configuration** — open the Bot panel to set hunt mode, potion thresholds, pickup filters, explore radius, safety settings, etc. Config is saved per-character as `config_<charname>.json` next to the exe.

6. **Start the bot** — hit Start. The bot will hunt monsters, loot items, use potions, and explore within the configured radius. It runs on the game thread via a VMT hook on `CHero::Process`.

### Hunt modes

| Mode | Description |
|------|-------------|
| Melee (0) | Jump to nearest monster, lock-attack. Uses weapon range from itemtype.json. |
| Ranged (1) | Same as melee but kites away if monsters get too close. Good for archers. |
| Scatter (2) | Positions for maximum AoE scatter hits using sector geometry prediction. |
| Magic (3) | Casts a magic spell at targets from safe distance, repositions if they close in. |

### Network proxy

The GUI can run a transparent proxy between the game client and server. It captures all packets for inspection in the Packet panel. The proxy handles the game's cipher (Blowfish + DH key exchange) transparently. Each account gets its own proxy on a separate port.

### Pathfinding

The pathfinder reads `.dmap` files to build a walkability grid. It uses A* with Chebyshev distance heuristic. Jump commands are validated against the grid — the bot won't try to jump through walls or into water. Max jump distance is 12 cells.

### Cross-map travel

Load `Gateways.json` (portal database) via the Cross-Map panel, then select a destination map. The system finds the shortest portal chain and walks through each one automatically. Unknown portals can be explored — the bot walks to each portal, steps on it, records the destination.

## Config files

- `config.json` — main config (client path, accounts, general settings). Saved next to the exe.
- `config_<charname>.json` — per-character bot config. Created when you start the bot.
- `Gateways.json` — portal database mapping source portals to destinations. Built by portal exploration or manually.

## Notes

- The GUI requires admin privileges (manifest flag) because it needs to inject into the game process.
- The DLL output name is `d3d11_config.dll` — this isn't actually a D3D11 config file, the name is just camouflage.
- The exe output name is `CQGraphicsConfig.exe` — same deal.
- HWID spoofing hooks WinAPI at the IAT level. It patches `GetUserNameA`, `GetVolumeInformationA`, `GetNativeSystemInfo`, and `GetComputerNameA`.
- The bot uses `SetCommand` via vtable for jumps instead of the game's `FN_JUMP` function, because the jump function has a side effect that corrupts the gold display.
- Static MSVC runtime is used (`/MT`) so you don't need to ship vcredist.

## Credits

- Manual map injector based on [Simple-Manual-Map-Injector](https://github.com/TheCruZ/Simple-Manual-Map-Injector) by TheCruZ.
