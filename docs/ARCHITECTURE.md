# Architecture & Technical Details

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│  Python Orchestrator (port 21338) — always alive                │
│  ├── switch_character    — exit → relaunch → select → enter     │
│  ├── new_game            — exit → relaunch → same char          │
│  ├── get_status          — game running? MCP alive? character?   │
│  ├── launch_game         — deploy DLL, start Game.exe            │
│  ├── proxy               — forward calls to in-game MCP         │
│  └── Survives game crashes and restarts                         │
├─────────────────────────────────────────────────────────────────┤
│  BH.dll (injected into Game.exe)                                │
│  ┌───────────────────────────────────────────────────────┐      │
│  │ Game Thread (D2's main loop, every frame)             │      │
│  │ ├── GameState::Update()    — snapshot player/units    │      │
│  │ ├── AutoPotion::Update()   — HP/MP threshold potions  │      │
│  │ ├── AutoPickup::Update()   — belt refill from ground  │      │
│  │ ├── AutoCast::Update()     — quick-cast combat/buffs  │      │
│  │ ├── GameCallQueue::Process — queued function calls    │      │
│  │ ├── GameNav::Update()      — menu navigation FSM      │      │
│  │ ├── StreamStats::Update()  — death/game tracking      │      │
│  │ └── GamePause::Check()     — pause/step/resume        │      │
│  ├───────────────────────────────────────────────────────┤      │
│  │ HTTP Thread (cpp-httplib, port 21337)                 │      │
│  │ ├── 80 MCP tool handlers   — read state, send packets │      │
│  │ ├── SSE sessions           — streaming responses      │      │
│  │ ├── Connection: close      — prevents exhaustion      │      │
│  │ └── Auto-restart loop      — recovers from crashes    │      │
│  ├───────────────────────────────────────────────────────┤      │
│  │ ImGui Thread (D3D9 separate window)                   │      │
│  │ ├── DebugPanel::Render()   — 10 tab panels            │      │
│  │ └── DPI-aware scaling      — per-monitor awareness    │      │
│  └───────────────────────────────────────────────────────┘      │
├─────────────────────────────────────────────────────────────────┤
│  Python Scripts                                                  │
│  ├── game_manager.py     — launch, navigate menus, select char  │
│  ├── orchestrator.py     — persistent MCP server on port 21338  │
│  ├── farming_loop.py     — automated WSK2 farming               │
│  ├── twitch_bot.py       — Twitch chat integration              │
│  └── build_and_deploy.py — compile → deploy → launch → verify   │
└─────────────────────────────────────────────────────────────────┘
```

## Threading Model

**Thread safety:** Game state is read by the HTTP thread via `std::mutex`-protected snapshots in `GameState`. `GameCallQueue` bridges HTTP→game thread for function calls that must run on the game thread (e.g., `SetUIVar`, PD2 stash tab handlers, `D2CLIENT_ExitGame`).

**Auto-systems** (AutoPotion, AutoPickup, AutoCast) run on the game thread every frame. They read game state directly (no mutex needed) and call game functions inline.

## C++ Modules

| Module | File | Purpose |
|--------|------|---------|
| AutoCast | AutoCast.cpp/h | Quick-cast combat skills, buff maintenance, immunity handling |
| AutoPickup | AutoPickup.cpp/h | Belt refill from ground items, tier-based potion matching |
| AutoPotion | AutoPotion.cpp/h | HP/MP/Rejuv threshold-based potion consumption |
| BH | BH.cpp/h | DLL entry point, WndProc hook, initialization |
| CrashCatcher | CrashCatcher.cpp/h | Vectored exception handler, register dump, stack trace |
| D2Handlers | D2Handlers.cpp/h | Game loop hook, pending exit handling |
| D2Helpers | D2Helpers.cpp/h | Unit name lookup, level name resolution |
| D2Ptrs | D2Ptrs.h | Function pointers and variable addresses for all D2 DLLs |
| D2Stubs | D2Stubs.cpp/h | ASM stubs for calling convention translation |
| DebugPanel | DebugPanel.cpp/h | ImGui 10-tab debug overlay |
| GameCallQueue | GameCallQueue.cpp/h | Thread-safe HTTP→game thread function call bridge |
| GameNav | GameNav.cpp/h | Menu navigation state machine, character selection |
| GamePause | GamePause.cpp/h | Pause/step/resume game loop |
| GameState | GameState.cpp/h | Thread-safe snapshots of player, belt, unit data |
| HookManager | HookManager.cpp/h | Microsoft Detours function hooking at runtime |
| McpServer | McpServer.cpp/h | HTTP+SSE MCP server with 80 tool handlers |
| MemWatch | MemWatch.cpp/h | Memory address watch list with change detection |
| PatchManager | PatchManager.cpp/h | Binary patch apply/revert/import/export |
| StreamStats | StreamStats.cpp/h | Session stats: deaths, kills, games, runs |
| StructRegistry | StructRegistry.cpp/h | Typed struct definitions for memory exploration |

## Debug Panel Tabs

| Tab | Contents |
|-----|----------|
| **Stream** | Live session stats: deaths, kills, loot, runs, session timer, status messages |
| **Player** | Full character sheet, resistances, breakpoints, MF/GF, belt grid, auto-potion/pickup/cast controls |
| **World** | Nearby units with HP, distance, immunities, boss/champion indicators |
| **Debug** | Function hook manager, call log, crash log with registers |
| **Memory** | Watch list with live value tracking and change detection |
| **Patches** | Binary patch manager with toggle, import/export |
| **Rendering** | Screen size, FPS, mouse, camera, automap, viewport |
| **Inventory** | Equipped gear (paper doll), backpack grid, cube grid |
| **Stash** | Stash grid with PD2 11-tab switcher (P/I-IX/M) |
| **Skills** | Left/right assigned skills, Fire/Lightning/Cold skill tables |

## MCP Tools (80 total)

### Core
`ping`, `get_game_info`, `get_game_state`, `get_controls`, `capture_screen`

### Player State
`get_player_state`, `get_belt_contents`, `get_skills`, `get_inventory`, `get_item_stats`, `get_stash_grid`, `get_cursor_item`

### World
`get_nearby_units`, `get_nearby_objects`, `get_level_exits`, `get_waypoints`, `get_collision_map`, `find_teleport_path`, `reveal_map`

### Combat & Movement
`cast_skill`, `switch_skill`, `attack_unit`, `walk_to`, `interact_entity`, `interact_object`, `pickup_item`, `use_item`, `drop_item`

### Automation
`get/set_auto_potion`, `get/set_auto_pickup`, `get/set_auto_cast`

### Items & Trade
`move_item`, `item_to_cursor`, `cursor_to_container`, `sell_item`, `open_stash`, `open_cube`, `switch_stash_tab`

### Navigation
`enter_game`, `exit_game`, `quit_game`, `select_character`, `launch_character`, `get_nav_status`, `use_waypoint`, `close_panels`, `is_panel_open`, `click_control`, `click_screen`, `wait_until`

### Memory & Debug
`read_memory`, `write_memory`, `read_struct`, `read_region`, `add_watch`, `remove_watch`, `get_watches`, `call_function`, `resolve_function`

### Structs
`list_struct_defs`, `get_struct_def`, `save_struct_defs`

### Hooking
`install_hook`, `remove_hook`, `list_hooks`, `get_call_log`

### Patches
`list_patches`, `apply_patch`, `toggle_patch`, `import_patches`, `export_patches`

### Game Control
`pause_game`, `resume_game`, `step_game`, `get_crash_log`

### Stream
`update_stream`, `get_stream_stats`

## AutoCast System

Runs on the game thread every frame. Quick-casts combat skills at nearby monsters and maintains buff timers.

```
AutoCast::Update() (every frame)
├── Check mana reserve threshold
├── Buff maintenance
│   ├── For each configured buff slot:
│   │   └── If 90% of duration elapsed → quick-cast recast
├── Combat (if enabled and not in town)
│   ├── Find target (configurable priority: nearest/lowest HP/boss first)
│   ├── Check target immunities
│   │   ├── Primary skill immune? → try backup skill
│   │   └── Both immune? → skip target
│   ├── Quick-cast sequence:
│   │   ├── Save current right-click skill
│   │   ├── Switch to combat skill (packet 0x3C)
│   │   ├── D2CLIENT_Attack(target) or cast_at_location
│   │   ├── Switch back to saved skill
│   │   └── Rate limit (configurable cooldown)
│   └── Cast-while-moving toggle
```

## Packet System

Movement, skills, and interactions use `D2NET_SendPacket`. Direct function calls (`D2CLIENT_Attack`, `D2CLIENT_ExitGame`) are preferred where available.

| ID | Size | Purpose | Format |
|----|------|---------|--------|
| 0x01 | 5 | Walk to location | `{01, WORD x, WORD y}` |
| 0x03 | 5 | Run to location | `{03, WORD x, WORD y}` |
| 0x02 | 9 | Walk to entity | `{02, DWORD type, DWORD id}` |
| 0x08 | 5 | Left skill at location (hold) | `{08, WORD x, WORD y}` |
| 0x0F | 5 | Right skill at location (hold) | `{0F, WORD x, WORD y}` |
| 0x06 | 9 | Left skill on unit | `{06, DWORD type, DWORD id}` |
| 0x0D | 9 | Right skill on unit | `{0D, DWORD type, DWORD id}` |
| 0x13 | 9 | Interact with entity | `{13, DWORD type, DWORD id}` |
| 0x16 | 13 | Pick up ground item | `{16, DWORD type, DWORD id, ...}` |
| 0x20 | 13 | Use item (inventory) | `{20, DWORD id, DWORD x, DWORD y}` |
| 0x26 | 13 | Use item (belt) | `{26, DWORD id, ...}` |
| 0x33 | 17 | Sell item to NPC | `{33, DWORD npc, DWORD item, DWORD tab, DWORD cost}` |
| 0x3C | 9 | Switch active skill | `{3C, WORD skill, 0x00, BYTE side, 0xFF×4}` |
| 0x49 | 9 | Use waypoint | `{49, DWORD wp_data, DWORD area_id}` |

## Direct Game Function Calls

Used instead of packets where available (more reliable, no network round-trip):

| Function | Address (PD2) | Convention | Purpose |
|----------|---------------|------------|---------|
| D2CLIENT_GetPlayerUnit | 0x6FB54D60 | stdcall | Get player unit pointer |
| D2CLIENT_Attack | 0x6FACC060 | stdcall(AttackStruct*) | Attack a target unit |
| D2CLIENT_ExitGame | 0x6FAF2850 | fastcall | Save and exit to main menu |
| D2CLIENT_SetUIVar | 0x6FB72790 | fastcall(var, set, unk) | Open/close UI panels |
| D2CLIENT_CloseNPCInteract | 0x6FAF4350 | fastcall | Close NPC trade panel |
| D2COMMON_GetUnitStat | ordinal | stdcall(unit, stat, layer) | Read unit stat value |
| D2NET_SendPacket | ordinal | stdcall(len, flag, data) | Send client→server packet |

## PD2-Specific Details

### Stash Tab Switching
PD2 has 11 tabs (Personal + Shared I-IX + Materials). Each tab has a dedicated `void(void)` handler in ProjectDiablo.dll at RVAs 0x1906c0-0x190900 (0x40 apart). Called via `GameCallQueue`.

### Panel Detection
| Panel | Detection Method |
|-------|-----------------|
| Inventory | `D2CLIENT_GetUIState(UI_INVENTORY)` or cube/stash/trade open |
| Character | `D2CLIENT_GetUIState(UI_CHARACTER)` |
| Skill Tree | `D2CLIENT_GetUIState(UI_SKILLTREE)` |
| Cube | `D2CLIENT_GetUIState(UI_CUBE)` |
| Stash | `D2CLIENT_GetUIState(UI_STASH)` or `panelState == 0x0C` |
| Trade | `panelState == 0x0D` |
| Waypoint | `g_dwData_add0 at 0x6FBAADD0 != 0` |
| Quest | `D2CLIENT_GetUIState(UI_QUEST)` |
| Chat | `D2CLIENT_GetUIState(UI_CHAT_CONSOLE)` |

### Waypoint System
- Waypoint table at `0x6FBACD8C` — 5-byte entries (DWORD areaId + BYTE unlocked)
- Entry count at `0x6FBACDDA`
- Current act tab at `0x6FBACDD6`
- Tab switching via `UpdateRoomLevelTracker` at `0x6FB59F40` (GameCallQueue)
- 39 waypoints across 5 acts

### Character Selection
- Character linked list head at `0x6FA65EC8` (D2Launch.dll)
- Entry size: 0xBC4 bytes, name at offset 0x000, next pointer at 0x34C
- Selected index at `0x6FA64DB0`
- Scroll offset at `0x6FA65ED4`
- Selection: write index directly, then `click_control` OK button

### Object Positions
Objects (type 2) use `ObjectPath` struct with DWORD positions at +0x0C/+0x10, NOT the player `Path` struct with WORD positions at +0x02/+0x06.

### Menu Navigation
PD2 adds a "SELECT GATEWAY" screen between Main Menu and Character Select. The navigator cancels the gateway, then retries Single Player.

## Python Scripts

| Script | Purpose |
|--------|---------|
| orchestrator.py | Persistent MCP server (port 21338) for game lifecycle management |
| game_manager.py | Launch, kill, deploy DLL, navigate menus, select character |
| farming_loop.py | Automated farming: waypoint → teleport → kill → loot → repeat |
| build_and_deploy.py | Compile → kill → deploy → launch → navigate → verify |
| twitch_bot.py | Twitch chat integration for stream commands |
| farm_wsk2.py | WSK2-specific farming script |
| teleport_path.py | Collision-based teleport pathfinding |
| permissions.py | Twitch permission tier system |
| task_queue.py | Prioritized task queue for stream requests |

## Orchestrator (port 21338)

Persistent Python MCP server that survives game restarts:

| Tool | Purpose |
|------|---------|
| switch_character | Exit game → relaunch → select character → enter game |
| new_game | Exit game → relaunch → same character → enter game |
| launch_game | Deploy DLL and start Game.exe |
| get_status | Game running? MCP alive? Character? Area? |
| proxy | Forward any tool call to the in-game MCP (port 21337) |

Exit flow: calls `D2CLIENT_ExitGame()` via in-game MCP for clean save, waits for process death, then relaunches.

## Memory Layout

### Key D2Client Globals
- `D2CLIENT_GetPlayerUnit`: offset 0xA4D60
- `D2CLIENT_pUnitTable`: offset 0x10A608 (128 buckets × 6 types)
- `D2WIN_FirstControl`: offset 0x214A0 (OOG control linked list)
- `g_dwGameModeState`: 0x6FBCC2CC
- `g_dwData_addc` (session active): 0x6FBAADDC

### Unit Hash Table
| Type | Buckets | Contents |
|------|---------|----------|
| 0 (players) | 0-127 | Player units |
| 1 (monsters) | 128-255 | NPCs and monsters |
| 2 (objects) | 256-383 | Waypoints, stash, shrines |
| 4 (items) | 512-639 | Ground items, inventory items |

### D2 Stat IDs
| ID | Stat | Notes |
|----|------|-------|
| 6/7 | life/max_life | value >> 8 for display |
| 31 | defense | |
| 39-46 | fire/light/cold/poison resist (and max) | |
| 80 | magic find | |
| 105 | faster cast rate | |
| 127 | all skills | |
| 214 | sockets | |
