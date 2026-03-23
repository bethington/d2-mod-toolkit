# d2-mod-toolkit

A comprehensive game instrumentation and automation platform for Project Diablo 2 Season 12, built as a BH.dll mod that loads via PD2's mod loader.

## What It Does

- **65+ MCP tools** accessible via HTTP on `localhost:21337` — read/write game state, move items, cast skills, navigate menus, travel waypoints
- **ImGui debug panel** — separate window showing player stats, nearby units, stash contents, monster immunities
- **Python automation scripts** — farming loops, stash organization, auto-vendoring, item evaluation
- **Full game lifecycle management** — launch, navigate menus, enter game, all without human interaction

## Quick Start

```bash
# 1. Build BH.dll
MSBuild BH/BH.vcxproj /p:Configuration=Release /p:Platform=Win32

# 2. Deploy + launch + navigate to game (all automatic)
python scripts/game_manager.py --character combustion

# 3. Farm (waypoint to area, kill monsters, return, vendor)
python scripts/farming_loop.py --area 35 --runs 3

# 4. Organize stash
python scripts/stash_organizer.py --organize

# 5. Evaluate and vendor junk items
python scripts/auto_vendor.py --all-stash
```

## Architecture

```
BH.dll (loaded by PD2)
├── McpServer       — HTTP+SSE MCP server on port 21337 (65+ tools)
├── GameState       — Thread-safe game state snapshots (player, belt, units)
├── DebugPanel      — ImGui+D3D9 window (7 tabs: Player, World, Debug, Memory, Patches, Rendering, Stash)
├── AutoPotion      — HP/MP/Rejuv auto-drink with configurable thresholds
├── AutoPickup      — Smart belt refill with snapshot-based slot matching
├── HookManager     — Microsoft Detours for 32 hook slots with call logging
├── CrashCatcher    — Vectored exception handler with register/stack dumps
├── PatchManager    — Binary patch tracking with toggle/import/export
├── GameNav         — Menu navigation state machine (Main Menu → Char Select → Difficulty → In Game)
├── GameCallQueue   — Queue function calls from HTTP thread to game thread
├── GamePause       — Game loop pause/step/resume for frame-by-frame debugging
├── MemWatch        — Monitor memory addresses for value changes
└── StructRegistry  — Typed struct definitions for memory exploration

Python Scripts
├── game_manager.py    — Kill/deploy/launch/navigate lifecycle
├── farming_loop.py    — Automated farming: waypoint → kill → loot → vendor
├── stash_organizer.py — Cross-tab stash reorganization with simulation
├── auto_vendor.py     — Evaluate items and sell junk to NPCs
├── item_evaluator.py  — Keep/vendor/review rules for PD2 items
└── knowledge/         — PD2 item values, class builds reference
```

## Documentation

- **[TOOLS.md](TOOLS.md)** — All 65+ MCP tools organized by category
- **[SCRIPTS.md](SCRIPTS.md)** — Python automation scripts usage guide
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — Technical details: threading, packets, memory layout
- **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)** — Known issues and workarounds

## Key Technical Details

- **Game**: Project Diablo 2 Season 12 (Diablo II 1.13c base)
- **DLL**: BH.dll — loaded by PD2's mod loader, compatible with PD2 BH fork
- **MCP Server**: HTTP+SSE on port 21337, JSON-RPC 2.0 protocol
- **Build**: Visual Studio 2022, Win32 Release configuration
- **Game Directory**: `C:\Diablo2\ProjectD2_dlls_removed`
- **Launch Command**: `Game.exe -3dfx` (must run from game directory)
