# Architecture & Technical Details

## Threading Model

```
Game Thread (D2's main loop)
├── GameState::Update()     — snapshots player/belt/units each frame
├── GameNav::Update()       — menu navigation state machine
├── GameCallQueue::Process  — executes queued function calls
├── AutoPotion::Update()    — checks HP/MP thresholds
├── AutoPickup::Update()    — scans nearby items for belt refill
└── GamePause::Check()      — pause/step/resume logic

HTTP Thread (cpp-httplib)
├── MCP tool handlers       — read game state, send packets
├── SSE sessions            — streaming responses
└── Auto-restart loop       — recovers from crashes

ImGui Thread (D3D9 separate window)
├── DebugPanel::Render()    — 7 tab panels
└── DPI-aware scaling       — per-monitor awareness
```

**Thread safety:** Game state is read by HTTP thread via `std::mutex`-protected snapshots. `GameCallQueue` bridges HTTP→game thread for function calls that must run on the game thread (like `SetUIVar` or PD2 tab handlers).

## Packet System

Items, movement, and interactions all use `D2NET_SendPacket`. Key packets:

| ID | Size | Purpose | Format |
|----|------|---------|--------|
| 0x01 | 5 | Walk to location | `{01, WORD x, WORD y}` |
| 0x03 | 5 | Run to location | `{03, WORD x, WORD y}` |
| 0x02 | 9 | Walk to entity | `{02, DWORD type, DWORD id}` |
| 0x05 | 5 | Left skill at location | `{05, WORD x, WORD y}` |
| 0x0C | 5 | Right skill at location | `{0C, WORD x, WORD y}` |
| 0x0D | 9 | Right skill on unit | `{0D, DWORD type, DWORD id}` |
| 0x13 | 9 | Interact with entity | `{13, DWORD type, DWORD id}` |
| 0x16 | 13 | Pick up ground item | `{16, DWORD type, DWORD id, ...}` |
| 0x17 | 5 | Drop item | `{17, DWORD id}` |
| 0x18 | 17 | Place item in container | `{18, DWORD id, DWORD x, DWORD y, DWORD dest}` |
| 0x19 | 5 | Pick item to cursor | `{19, DWORD id}` |
| 0x33 | 17 | Sell item to NPC | `{33, DWORD npc, DWORD item, DWORD tab, DWORD cost}` |
| 0x49 | 9 | Use waypoint | `{49, DWORD wp_data, DWORD area_id}` |

## PD2-Specific Details

### Stash Tab Switching
PD2 has 11 tabs (Personal + Shared I-IX + Materials). Each tab has a dedicated void(void) handler function in ProjectDiablo.dll:

```
Tab 0-9: RVAs 0x1906c0 through 0x190900 (0x40 apart)
Materials (tab 10): Inline ASM sending packet {0x55, 0x0B} via dispatcher
```

Handlers check `*DAT_10410688 == 0xC` (stash open) before switching. Called via `GameCallQueue` from the HTTP thread.

### Panel State Detection
- Stash open: `*DAT_10410688 == 0x0C`
- Trade open: `*DAT_10410688 == 0x0D`
- Waypoint open: `g_dwData_add0 at 0x6FBAADD0 != 0`
- DAT_10410688 is at ProjectDiablo.dll + 0x410688 (pointer to D2Client global)

### Object Positions
Objects (type 2) use `ObjectPath` struct with DWORD positions at +0x0C/+0x10, NOT the player `Path` struct with WORD positions at +0x02/+0x06. This affects stash/waypoint/NPC position reading.

### Menu Navigation
PD2 adds a "SELECT GATEWAY" screen between Main Menu and Character Select. The navigator cancels the gateway, then retries Single Player to bypass it.

Difficulty buttons appear as an overlay on Character Select (32 controls total vs 26 for char select alone).

## Memory Layout

### Key D2Client Globals
- `D2CLIENT_GetPlayerUnit`: offset 0xA4D60 (1.13c)
- `D2CLIENT_pUnitTable`: offset 0x10A608 (unit hash table, 128 buckets × 6 types)
- `D2WIN_FirstControl`: offset 0x214A0 (OOG control linked list)

### Unit Hash Table
Type 0 (players): buckets 0-127
Type 1 (monsters): buckets 128-255
Type 2 (objects): buckets 256-383
Type 4 (items): buckets 512-639

### Item Storage Locations
- `ItemLocation 0` = inventory
- `ItemLocation 3` = cube
- `ItemLocation 4` = stash (current tab only)

### D2 Stat IDs (commonly used)
- 6/7: life/max_life (value >> 8 for display)
- 31: defense
- 39-46: fire/light/cold/poison resist (and max)
- 80: magic find
- 105: faster cast rate
- 127: all skills
- 188: skill tab (grand charm skillers)
- 214: sockets
