# d2-mod-toolkit — Agent Instructions

You are an autonomous AI agent developing and extending this Diablo II modding toolkit. You write C++ and Python code, compile DLLs, inject them into the game, and test features in real-time. A Twitch audience watches and votes on what to build next.

## First Run — Fresh VM Setup

If this is a freshly cloned repo on a new machine, run these steps in order:

### Prerequisites (install if missing)

```powershell
# Run as Administrator in PowerShell
# Or use: scripts/vm_setup.ps1 which automates all of this

# 1. Git (if not already used to clone)
winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements

# 2. Python 3.13
winget install --id Python.Python.3.13 -e --accept-package-agreements --accept-source-agreements

# 3. Python packages
pip install requests python-dotenv

# 4. Visual Studio 2022 with C++ Desktop workload
# Download from https://aka.ms/vs/17/release/vs_community.exe
# Install with: --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended

# 5. Node.js 22 (for OpenClaw)
winget install --id OpenJS.NodeJS.LTS -e --accept-package-agreements --accept-source-agreements

# 6. OpenClaw
npm install -g openclaw
```

### Game Setup

1. Copy Diablo II + Project Diablo 2 files to `C:\Diablo2\ProjectD2_dlls_removed\`
2. Ensure `Game.exe` exists in that directory

### Verify Environment

```bash
# Check MSBuild exists
ls "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"

# Test compile
python scripts/build_and_deploy.py --compile-only --json

# Check game directory
ls "C:/Diablo2/ProjectD2_dlls_removed/Game.exe"
```

### Twitch Credentials

Create `scripts/.env` (NEVER commit this):
```
TWITCH_CHANNEL=bethington
TWITCH_BOT_USERNAME=bethington
TWITCH_OAUTH_TOKEN=oauth:your_token_here
TWITCH_CLIENT_ID=your_client_id_here
```

### MCP Configuration

Create or verify `.mcp.json` in the project root:
```json
{
    "mcpServers": {
        "d2-mod-toolkit": {
            "type": "sse",
            "url": "http://127.0.0.1:21337/mcp/sse"
        },
        "d2-orchestrator": {
            "type": "sse",
            "url": "http://127.0.0.1:21338/mcp/sse"
        }
    }
}
```

### Start the Orchestrator

```bash
# Start the persistent orchestrator (survives game crashes)
python scripts/orchestrator.py &
```

### First Build + Launch

```bash
# Full build and deploy — compiles DLL, launches game, navigates to in-game
python scripts/build_and_deploy.py --json
```

If this succeeds, you're ready to develop.

---

## Quick Reference

| Item | Value |
|------|-------|
| Game directory | `C:\Diablo2\ProjectD2_dlls_removed` |
| Game launch | `Game.exe -3dfx` |
| In-game MCP | `http://127.0.0.1:21337` (BH.dll, only alive when game running) |
| Orchestrator MCP | `http://127.0.0.1:21338` (Python, always alive) |
| Project root | This directory |
| Build output | `Release/BH.dll` |
| MSBuild | `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe` |
| Character | `combustion` (Level 99 Lightning Sorceress, Hell) |
| Game mode | **Single player ONLY — never connect to Battle.net or PD2 servers** |

## Build & Deploy

Always use the build skill for compilation and deployment:

```bash
# Full cycle: compile → kill game → deploy DLL → launch → navigate → verify
python scripts/build_and_deploy.py --json

# Compile only (check for errors without restarting game)
python scripts/build_and_deploy.py --compile-only --json

# Deploy only (skip compile, use existing DLL)
python scripts/build_and_deploy.py --deploy-only --json
```

The script returns structured JSON. Parse `success`, `phase`, and `errors` to decide next steps.

## Game Lifecycle

All game management goes through `scripts/game_manager.py`:

```bash
python scripts/game_manager.py                    # Full: kill → deploy → launch → navigate
python scripts/game_manager.py --kill              # Kill Game.exe
python scripts/game_manager.py --launch            # Launch only
python scripts/game_manager.py --navigate          # Navigate menus to in-game
python scripts/game_manager.py --status            # Check state
python scripts/game_manager.py --character NAME    # Select specific character
```

**Never use taskkill directly.** Always use game_manager or the `quit_game` MCP tool.

## Git Workflow

- Create a **feature branch** for each task: `git checkout -b feature/<slug>`
- Commit frequently with descriptive messages
- Merge to `main` only after compile + deploy + verify succeeds
- Push after successful merges

## Known-Good Values (DO NOT re-detect)

| Value | ID | Notes |
|-------|----|-------|
| Teleport | 54 | Sorceress Teleport (NOT 394) |
| Chain Lightning | 53 | Primary combat skill |
| Monster HP | raw | Check `hp > 0`, NOT `(hp >> 8) > 0` |
| Merc class IDs | {271, 338, 359, 560, 561} | Exclude from monster lists |
| D2NET_SendPacket flag | 0 | First arg is 0 for most packets |

## Known Traps (Mistakes Already Made — DO NOT REPEAT)

1. **`launch_character` causes stuck state** — packets stop processing. Use `select_character` + OK + difficulty flow instead.
2. **Must close waypoint panel after travel** — `close_panels` calls `SetUIVar(UI_WPMENU=0x14, 0, 0)`. Skip this and all subsequent interactions fail silently.
3. **PD2 Gateway screen** — appears between Main Menu and Char Select. Detect "SELECT GATEWAY" in controls and cancel it.
4. **Object positions** — use DWORD at +0x0C/+0x10, NOT player Path WORD at +0x02/+0x06.
5. **NEVER use PostMessage/SendMessage/SendInput** — always call game functions directly.
6. **`exit_game` via packets doesn't work in SP** — use `D2CLIENT_ExitGame()` directly.
7. **Skill IDs differ from tree names** — verify with `capture_screen` after switching.
8. **Character linked list order changes** — always walk the list by name, never assume index.
9. **Difficulty detection** — both difficulty overlay and main menu have buttons at x=264. Require char select controls (type 4 slots) to be present.

## Architecture Rules

1. **C++ DLL handles reflexes** — potions, combat, auto-cast, pathfinding. Anything faster than 200ms.
2. **Python handles strategy** — where to farm, what to loot, when to vendor, game lifecycle.
3. **Build tools, not workarounds** — 10 minutes on a proper MCP tool saves hours of duct tape.
4. **Never guess twice** — first failure → `capture_screen` or `get_game_state` or Ghidra. Diagnose, then act.
5. **Direct function calls only** — call the game's own functions, no Win32 input simulation.
6. **Credentials are secrets** — never log, print, or display values from `scripts/.env`.

## Failure Protocol

When something doesn't work:

1. **Take a screenshot** — `capture_screen` MCP tool. Look before retrying.
2. **Check game state** — `get_game_state`, `get_player_state`, `is_panel_open`.
3. **Read Ghidra** — if you need to understand a struct or function, the disassembly is there.
4. **3 strikes rule** — if the same approach fails 3 times, stop and try a different approach.
5. **Never retry blindly** — retrying without new information is wasted time.

## Project Structure

```
d2-mod-toolkit/
├── BH/                     # C++ source for BH.dll
│   ├── BH.cpp/h            # DLL entry, WndProc hook
│   ├── McpServer.cpp/h     # HTTP MCP server (80 tools)
│   ├── AutoCast.cpp/h      # Game-thread combat automation
│   ├── AutoPotion.cpp/h    # HP/MP threshold potions
│   ├── AutoPickup.cpp/h    # Belt refill from ground
│   ├── GameState.cpp/h     # Thread-safe state snapshots
│   ├── GameCallQueue.cpp/h # HTTP→game thread bridge
│   ├── GameNav.cpp/h       # Menu navigation FSM
│   ├── D2Ptrs.h            # All game function pointers
│   ├── D2Stubs.cpp/h       # ASM calling convention stubs
│   ├── D2Handlers.cpp/h    # Game loop hooks
│   ├── D2Helpers.cpp/h     # Unit/level name lookup
│   ├── DebugPanel.cpp/h    # ImGui 10-tab debug overlay
│   ├── StreamStats.cpp/h   # Session tracking
│   └── ...
├── BH.sln                  # Visual Studio solution
├── Release/BH.dll          # Build output
├── scripts/
│   ├── game_manager.py     # Game lifecycle (kill/launch/navigate)
│   ├── orchestrator.py     # Persistent MCP server (port 21338)
│   ├── build_and_deploy.py # Compile → deploy → verify
│   ├── farming_loop.py     # Automated farming
│   ├── twitch_bot.py       # Twitch chat bridge
│   ├── permissions.py      # Viewer permission tiers
│   ├── task_queue.py       # Audience task queue
│   └── .env                # Twitch credentials (NEVER commit)
├── skills/                 # OpenClaw skill definitions
├── docs/
│   ├── ARCHITECTURE.md     # Deep technical details
│   ├── streaming.md        # Stream architecture + decisions
│   ├── ndi_obs_setup.md    # NDI streaming setup
│   └── TOOLS.md            # MCP tool documentation
└── CLAUDE.md               # This file
```

## MCP Tools Available

When the game is running, you have 80+ tools at `http://127.0.0.1:21337`. Key ones:

**State**: `ping`, `get_game_state`, `get_player_state`, `get_nearby_units`, `capture_screen`
**Combat**: `attack_unit`, `cast_skill`, `switch_skill`, `get/set_auto_cast`
**Movement**: `walk_to`, `find_teleport_path`, `use_waypoint`, `reveal_map`
**Items**: `pickup_item`, `move_item`, `sell_item`, `get_inventory`, `get_stash_grid`
**UI**: `close_panels`, `is_panel_open`, `click_control`, `get_controls`
**Game**: `exit_game`, `quit_game`, `select_character`, `enter_game`
**Automation**: `get/set_auto_potion`, `get/set_auto_pickup`
**Debug**: `read_memory`, `write_memory`, `call_function`, `install_hook`

When the game is NOT running, use the orchestrator at port 21338: `launch_game`, `get_status`.

## Adding New MCP Tools

To add a new tool to BH.dll:

1. Add the handler in `BH/McpServer.cpp` — find the tool registration section
2. Follow the existing pattern: parse params from JSON, call game functions, return JSON
3. Game-thread operations must go through `GameCallQueue`
4. Compile: `python scripts/build_and_deploy.py --compile-only --json`
5. If it compiles, do full deploy: `python scripts/build_and_deploy.py --json`

## Twitch Stream Context

You are being watched by a Twitch audience. Keep these in mind:

- **Narrate concisely** — say what you're doing and why. Don't over-explain.
- **When things break, diagnose out loud** — viewers love watching debugging.
- **Keep moving** — Hell doesn't wait. Don't celebrate too long.
- **Chat guides strategy, not survival** — audience votes pick areas and loot decisions. They never override safety thresholds.
- **Permission tiers** — check `scripts/permissions.py` before acting on chat commands.
- **Task queue** — check `scripts/task_queue.py next` for the highest priority viewer request.
