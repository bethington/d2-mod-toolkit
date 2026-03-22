# d2-mod-toolkit

A comprehensive game instrumentation and modding toolkit for **Project Diablo 2 Season 12** (1.13c base). Built as a fork of [PD2 BH](https://github.com/Project-Diablo-2/BH) with all original features preserved.

## Features

### ImGui Debug Panel
Separate D3D9 window with 6 tabs:
- **Player** — Full stats (matching BH StatsDisplay), breakpoints, belt contents, auto-potion/pickup controls
- **World** — Nearby units with names, HP, distance, boss/champion indicators
- **Debug** — Function hook manager, call log, crash log with registers/stack trace
- **Memory** — Watch list with live value tracking and change detection
- **Patches** — Binary patch manager with toggle, import/export
- **Rendering** — Screen size, FPS, mouse, camera, automap, viewport debug

### Embedded MCP Server (43 tools)
HTTP+SSE server on `localhost:21337` for AI agent integration via Model Context Protocol.

| Category | Tools |
|----------|-------|
| Core | `ping`, `get_game_info`, `get_game_state`, `get_controls` |
| Game State | `get_player_state`, `get_belt_contents`, `get_nearby_units`, `get_inventory` |
| Memory | `read_memory`, `write_memory`, `add_watch`, `remove_watch`, `get_watches` |
| Structs | `list_struct_defs`, `get_struct_def`, `read_struct`, `read_region`, `save_struct_defs` |
| Functions | `call_function`, `resolve_function` |
| Gameplay | `get/set_auto_potion`, `get/set_auto_pickup` |
| Hooking | `install_hook`, `remove_hook`, `list_hooks`, `get_call_log` |
| Navigation | `enter_game`, `exit_game`, `quit_game`, `get_nav_status` |
| Debugging | `get_crash_log`, `pause_game`, `resume_game`, `step_game` |
| Patches | `list_patches`, `apply_patch`, `toggle_patch`, `import_patches`, `export_patches` |

### Auto-Potion
- Configurable HP/MP/Rejuv thresholds
- Name-based potion detection (works with PD2's remapped item codes)
- Skips in town, respects potion cooldowns

### Smart Belt Auto-Pickup
- Snapshots belt layout, enforces item types per column
- Tier-based matching: same-or-better potions preferred
- Fallback one tier when on last potion
- TP/ID scroll pickup when tomes not full
- 3-second blacklist on failed pickups

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

### Struct Registry & Explorer
- 11 built-in D2 struct definitions (UnitAny, Path, Inventory, etc.)
- `read_struct` reads typed memory with field names and pointer following
- `read_region` classifies unknown memory (pointer/string/int/zero)
- Python scripts for autonomous struct discovery

## Building

Requires Visual Studio 2022 with C++ Desktop workload.

```
cd d2-mod-toolkit
MSBuild BH.sln /p:Configuration=Release /p:Platform=Win32 /t:Rebuild
```

Output: `Release/BH.dll`

## Installation

1. Copy `BH.dll` to your PD2 game directory
2. PD2's loader auto-discovers and loads it
3. Launch with `Game.exe -3dfx`
4. Debug panel appears alongside the game window

## MCP Configuration

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

## Python Scripts

```bash
# List known struct definitions
python scripts/struct_explorer.py list

# Explore a struct from a live address
python scripts/struct_explorer.py explore --address 0x231A8A00 --type UnitAny --depth 2

# Discover unknown memory layout
python scripts/struct_explorer.py propose --address 0x22394000 --size 128 --name PlayerDataNew

# Auto-discover from known anchor points
python scripts/auto_discover.py --anchor PlayerUnit --depth 2 --output structs/

# Export struct definitions
python scripts/struct_explorer.py export --output structs/ --format both
```

## Configuration (BH.json)

Settings are persisted to `BH.json` in the game directory:
- `debug_panel` — position, size, DPI preset, MCP port
- `auto_potion` — enabled, HP/MP/rejuv thresholds, cooldown
- `auto_pickup` — enabled, range, TP/ID scroll toggles, belt snapshot

## License

Based on PD2 BH (Apache 2.0). Additional code by Claude Opus 4.6.
