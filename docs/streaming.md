# Claude vs Diablo II: Twitch-Driven Autonomous Development

## Overview

An autonomous AI agent develops and extends the d2-mod-toolkit project live on Twitch, with the audience guiding priorities. The agent writes C++ and Python code, compiles DLLs, injects them into Diablo II, and tests features in real-time — all while viewers watch and vote on what to build next.

**Stream**: twitch.tv/bethington
**Model**: Claude Opus
**Game**: Diablo II: Project Diablo 2, Single Player ONLY

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  HOST MACHINE                                               │
│  ┌──────────┐  ┌──────────┐                                 │
│  │ OBS      │◄─┤ NDI Feed │◄──── from VM                    │
│  │ Stream   │  └──────────┘                                 │
│  │ Engine   │  + vote overlay                               │
│  │          │  + queue panel                                │
│  └──────────┘  + debug stats                                │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  WINDOWS VM (isolated)                                      │
│                                                             │
│  ┌───────────────────────────────────────────────────┐      │
│  │ Claude Code Agent (24/7)                          │      │
│  │ ├─ Model: Claude Opus                             │      │
│  │ ├─ Channel: Twitch (bethington)                   │      │
│  │ ├─ MCP Client → orchestrator:21338                │      │
│  │ │              → in-game MCP:21337                │      │
│  │ │              → Ghidra MCP                       │      │
│  │ └─ Full autonomy: edit, compile, inject, push     │      │
│  └──────────────┬────────────────────────────────────┘      │
│                 │                                            │
│  ┌──────────────▼────────────────────────────────────┐      │
│  │ Python Orchestrator (port 21338, always alive)    │      │
│  │ ├─ switch_character, new_game, get_status         │      │
│  │ ├─ launch_game, proxy                             │      │
│  │ └─ Survives game crashes                          │      │
│  └──────────────┬────────────────────────────────────┘      │
│                 │                                            │
│  ┌──────────────▼────────────────────────────────────┐      │
│  │ BH.dll MCP Server (port 21337)                    │      │
│  │ ├─ 80 game control tools                          │      │
│  │ ├─ Auto-cast, auto-potion, auto-pickup            │      │
│  │ ├─ Screen capture, memory read/write              │      │
│  │ └─ Running inside Diablo 2 process                │      │
│  └──────────────┬────────────────────────────────────┘      │
│                 │                                            │
│  ┌──────────────▼──────┐  ┌───────────────────┐            │
│  │ Diablo 2 (PD2 SP)  │  │ Ghidra MCP        │            │
│  │ Single player only  │  │ Reverse eng tools │            │
│  └─────────────────────┘  └───────────────────┘            │
│                                                             │
│  ┌───────────────────────────────────────────────────┐      │
│  │ NDI output → host                                 │      │
│  └───────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────┘
```

---

## Current Capabilities

### Working Systems
- **Teleport**: Skill 54 (Sorceress Teleport) — verified with before/after screenshots
- **Combat**: `D2CLIENT_Attack()` direct function call — kills full packs in one shot
- **Auto-Cast**: Quick-cast system on game thread — configurable skills, immunity handling, buff maintenance
- **Skill Switching**: Packet 0x3C with verification loop — confirms skill actually changed
- **Item Pickup**: `pickup_item` MCP tool — picks up ground items by unit ID
- **Screen Capture**: `capture_screen` — half-res PNG for visual verification
- **Character Selection**: Direct memory write to character index — works with 108+ characters
- **Game Exit**: `D2CLIENT_ExitGame()` — clean save and exit to main menu
- **Orchestrator**: Python MCP (port 21338) — switch characters, new games, survives crashes
- **Waypoint Travel**: All 5 acts with tab switching and unlock detection
- **Panel Detection**: Inventory, cube, stash, waypoint, character, skill tree, quest, chat

### Known-Good Skill IDs (from skills.txt)
| ID | Skill | Class | Notes |
|----|-------|-------|-------|
| 0 | Attack | All | Basic attack |
| 36 | Fire Bolt | Sorceress | Fire tree |
| 38 | Charged Bolt | Sorceress | Lightning tree |
| 47 | Fire Ball | Sorceress | Fire tree |
| 48 | Nova | Sorceress | Lightning tree |
| 49 | Lightning | Sorceress | Lightning tree |
| 53 | Chain Lightning | Sorceress | Lightning tree, 20 base |
| 54 | Teleport | Sorceress | Lightning tree — THIS IS TELEPORT |
| 57 | Thunder Storm | Sorceress | Lightning tree, buff |
| 58 | Energy Shield | Sorceress | Lightning tree, buff |
| 394 | (PD2 custom) | Sorceress | Unknown tree — NOT teleport despite earlier belief |

### Known Traps (Mistakes Already Made)
- `launch_character` causes stuck state — use `select_character` + OK + difficulty flow
- Must close waypoint panel after travel — `close_panels` calls `SetUIVar(UI_WPMENU=0x14, 0, 0)`
- PD2 Gateway screen between Main Menu and Char Select — detect "SELECT GATEWAY" and cancel
- Objects use DWORD positions at +0x0C/+0x10, NOT player Path WORD at +0x02/+0x06
- **NEVER use PostMessage/SendMessage/SendInput** — always use direct game function calls
- `exit_game` via packets doesn't work in single player — use `D2CLIENT_ExitGame()` directly
- Skill IDs differ from what `get_skills` tree names suggest — verify with `capture_screen`
- `D2NET_SendPacket` arg1 should be 0 for most packets (was incorrectly set to 1)
- Character linked list order changes each game launch — always walk the list by name

### Architecture Rules
1. **C++ DLL handles reflexes** — potions, combat, auto-cast — anything faster than 200ms
2. **Python handles strategy** — where to farm, what to loot, when to vendor, game lifecycle
3. **Build tools, not workarounds** — 10 minutes on a proper MCP tool saves hours of duct tape
4. **Never guess twice** — first failure → `capture_screen` or `get_game_state` or Ghidra
5. **Direct function calls only** — no Win32 input simulation, call the game's own functions

---

## Decisions

| Decision | Choice |
|----------|--------|
| Session model | Hybrid — continuous backlog, audience can interrupt/reprioritize |
| Audience input | Queue with paid/sub priority boosting |
| Model | Claude Opus |
| Isolation | Dedicated Windows VM |
| Stream capture | NDI from VM to host OBS |
| Failure recovery | Autonomous — 3 failed attempts triggers escalation |
| Owner role | Producer/moderator — intervene when off the rails |
| Autonomy | Full — edit, compile, inject, push to git |
| Git workflow | Feature branch per task, merge on success |
| Game boundary | Single player ONLY — never Battle.net/PD2 servers |
| Combat system | Direct function calls (`D2CLIENT_Attack`) not packets |
| Game exit | `D2CLIENT_ExitGame()` via GameCallQueue, not packets |
| Game lifecycle | Python orchestrator (port 21338) manages launch/exit/restart |
| Skill verification | Send 0x3C packet, then verify skill changed by reading player state |

---

## Permission Tiers

| Tier | Who | Capabilities |
|------|-----|-------------|
| **Owner** | `bethington` | Full control — direct commands, override queue, approve/ban |
| **Trusted** | Mods, whitelisted | Submit features, vote high weight, request code changes |
| **Subscriber** | Twitch subs | Boosted votes, submit requests |
| **Viewer** | Approved | Vote on options, info commands (!stats, !area) |
| **Banned** | Blocked | All messages ignored |

---

## Credentials

Stored in `scripts/.env`:
- `TWITCH_CHANNEL`, `TWITCH_BOT_USERNAME`, `TWITCH_OAUTH_TOKEN`, `TWITCH_CLIENT_ID`

**NEVER log, print, display, or include credential values in any output.**

---

## Blacklist (Off-Limits)

The agent must refuse:
- Anything that connects to Battle.net or PD2 servers
- Griefing, spam, or harassment tools
- File access outside the project directory
- Credential exposure
- Lowering safety thresholds below safe minimums
