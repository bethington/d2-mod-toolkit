# d2-mod-toolkit

A comprehensive game instrumentation and modding toolkit for **Project Diablo 2 Season 12** (1.13c base). Built as a fork of [PD2 BH](https://github.com/Project-Diablo-2/BH) with all original features preserved.

## Features

### ImGui Debug Panel
Separate D3D9 window with 10 tabs:
- **Stream** — Live session stats overlay: deaths, kills, loot, runs, session timer
- **Player** — Full character sheet, breakpoints, belt, auto-potion/pickup/cast controls
- **World** — Nearby units with HP, distance, immunities, boss/champion indicators
- **Debug** — Function hook manager, call log, crash log with registers/stack trace
- **Memory** — Watch list with live value tracking and change detection
- **Patches** — Binary patch manager with toggle, import/export
- **Rendering** — Screen size, FPS, mouse, camera, automap, viewport debug
- **Inventory** — Equipped gear, backpack grid, cube grid
- **Stash** — PD2 11-tab stash grid viewer (Personal + Shared I-IX + Materials)
- **Skills** — Left/right assigned skills, Fire/Lightning/Cold skill tree tables

### Embedded MCP Server (80 tools)
HTTP+SSE server on `localhost:21337` for AI agent integration via Model Context Protocol.

| Category | Tools |
|----------|-------|
| Core | `ping`, `get_game_info`, `get_game_state`, `get_controls`, `capture_screen` |
| Player | `get_player_state`, `get_belt_contents`, `get_skills`, `get_inventory`, `get_item_stats`, `get_stash_grid`, `get_cursor_item` |
| World | `get_nearby_units`, `get_nearby_objects`, `get_level_exits`, `get_waypoints`, `get_collision_map`, `find_teleport_path`, `reveal_map` |
| Combat | `cast_skill`, `switch_skill`, `attack_unit`, `walk_to`, `interact_entity`, `interact_object`, `pickup_item` |
| Items | `use_item`, `drop_item`, `move_item`, `item_to_cursor`, `cursor_to_container`, `sell_item` |
| Automation | `get/set_auto_potion`, `get/set_auto_pickup`, `get/set_auto_cast` |
| Navigation | `enter_game`, `exit_game`, `quit_game`, `select_character`, `use_waypoint`, `close_panels`, `is_panel_open` |
| Memory | `read_memory`, `write_memory`, `read_struct`, `read_region`, `add_watch`, `remove_watch`, `get_watches` |
| Functions | `call_function`, `resolve_function` |
| Structs | `list_struct_defs`, `get_struct_def`, `save_struct_defs` |
| Hooking | `install_hook`, `remove_hook`, `list_hooks`, `get_call_log` |
| Patches | `list_patches`, `apply_patch`, `toggle_patch`, `import_patches`, `export_patches` |
| Game Control | `pause_game`, `resume_game`, `step_game`, `get_crash_log` |
| Stream | `update_stream`, `get_stream_stats` |
| Stash | `open_stash`, `open_cube`, `switch_stash_tab` |

### Python Orchestrator (port 21338)
Persistent MCP server that survives game crashes and restarts:
- `switch_character` — exit current game, relaunch with a different character
- `new_game` — exit and re-enter with the same character (fresh map)
- `get_status` — game running? MCP alive? what character? what area?
- `launch_game` — deploy DLL and start Game.exe
- `proxy` — forward any tool call to the in-game MCP

### Auto-Cast System
Game-thread combat automation with configurable behavior:
- **Quick-cast**: switch to combat skill → attack → switch back (like PD2 Quick Cast)
- **Immunity handling**: skip immune monsters, try backup skill
- **Configurable targeting**: nearest, lowest HP, highest HP, boss priority
- **Buff maintenance**: auto-recast buffs (Thunder Storm, Energy Shield, etc.) at 90% duration
- **Mana reserve**: configurable threshold to preserve mana for teleport
- **Cast while moving**: toggle between stop-and-fight vs cast-between-teleports

### Auto-Potion
- Configurable HP/MP/Rejuv thresholds
- Name-based potion detection (works with PD2's remapped item codes)
- Skips in town, respects potion cooldowns

### Smart Belt Auto-Pickup
- Snapshots belt layout, enforces item types per column
- Tier-based matching: same-or-better potions preferred
- TP/ID scroll pickup when tomes not full
- 3-second blacklist on failed pickups

### Screen Capture
- `capture_screen` MCP tool returns game window screenshot as PNG
- Half-resolution for fast transfer (~50-100KB)
- Used for visual verification of skill switching, teleporting, combat

### Direct Game Function Calls
All game interactions use direct function calls — no PostMessage, SendMessage, or SendInput:
- `D2CLIENT_Attack()` for combat
- `D2CLIENT_ExitGame()` for clean save-and-exit
- `click_control` calls OnPress handlers directly for UI
- Packet-based movement and skill switching with verification

### Function Hooking (Microsoft Detours)
- Hook any game function at runtime via MCP
- Capture return values with naked assembly trampolines
- Call log ring buffer (10,000 entries)
- 32 simultaneous hook slots

### Game-Thread-Safe Function Calls
- `call_function` executes on game thread via `GameCallQueue`
- `resolve_function` resolves DLL ordinals to addresses
- SEH crash protection with register dump

### Crash Catcher
- Vectored exception handler captures fatal exceptions
- Full register state (EAX-EIP, EFLAGS)
- Stack trace via EBP chain walk (16 frames)
- Module identification (DLL + offset)

### Waypoint System
- Read all 39 waypoints across 5 acts with unlock status
- Act tab switching via `UpdateRoomLevelTracker`
- Waypoint travel with automatic panel close

### Character Selection
- Full character list enumeration (reads D2Launch linked list)
- Select any character by name (direct memory write, no input simulation)
- Works with 100+ characters, no scrolling issues

## Building

Requires Visual Studio 2022 with C++ Desktop workload.

```
cd d2-mod-toolkit
MSBuild BH.sln /p:Configuration=Release /p:Platform=Win32 /t:Rebuild
```

Output: `BH/Release/BH.dll`

## Installation

1. Copy `BH.dll` to your PD2 game directory
2. PD2's loader auto-discovers and loads it
3. Launch with `Game.exe -3dfx`
4. Debug panel appears alongside the game window

## MCP Configuration

### In-Game MCP (BH.dll)
Add to `.mcp.json`:
```json
{
    "mcpServers": {
        "d2-mod-toolkit": {
            "type": "sse",
            "url": "http://127.0.0.1:21337/mcp/sse"
        }
    }
}
```

### Python Orchestrator
```bash
python scripts/orchestrator.py &
```
Runs on port 21338. Add to `.mcp.json`:
```json
{
    "mcpServers": {
        "d2-orchestrator": {
            "type": "sse",
            "url": "http://127.0.0.1:21338/mcp/sse"
        }
    }
}
```

## Python Scripts

| Script | Purpose |
|--------|---------|
| `orchestrator.py` | Persistent MCP server for game lifecycle management |
| `game_manager.py` | Launch, kill, deploy, navigate menus, select character |
| `farming_loop.py` | Automated farming: waypoint → teleport → kill → loot → repeat |
| `build_and_deploy.py` | Compile → deploy → launch → verify (structured JSON output) |
| `twitch_bot.py` | Twitch chat integration for stream viewer commands |
| `farm_wsk2.py` | WSK2-specific farming with area-specific logic |
| `teleport_path.py` | Collision-based teleport pathfinding |

## Configuration (BH.json)

Settings persisted to `BH.json` in the game directory:
- `debug_panel` — position, size, DPI preset, MCP port
- `auto_potion` — enabled, HP/MP/rejuv thresholds, cooldown
- `auto_pickup` — enabled, range, TP/ID scroll toggles, belt snapshot
- `auto_cast` — enabled, skills, backup skills, range, priority, mana reserve, buff slots

## License

Based on PD2 BH (Apache 2.0). Additional code by Claude Opus 4.6.
