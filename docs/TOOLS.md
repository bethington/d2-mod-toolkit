# MCP Tools Reference

HTTP server at `localhost:21337`. All tools called via JSON-RPC 2.0 POST to `/mcp`.

```bash
curl -s http://localhost:21337/mcp -X POST -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"TOOL","arguments":{...}}}'
```

## Game Lifecycle

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_game_state` | Current state: in_game/menu/loading, area, difficulty, frame | |
| `enter_game` | Navigate menus to enter a game | `character`, `difficulty` |
| `get_nav_status` | Check menu navigation progress | |
| `exit_game` | Save and return to character select | |
| `quit_game` | Save and terminate Game.exe | |

## Movement & Interaction

| Tool | Description | Key Args |
|------|-------------|----------|
| `walk_to` | Walk/run to world coordinates | `x`, `y`, `run` |
| `interact_entity` | Walk to entity + interact + verify panel opened | `unit_id`, `unit_type`, `expected_panel`, `timeout_ms` |
| `interact_object` | Send interact packet (raw, no verification) | `unit_id`, `unit_type` |
| `use_waypoint` | Travel via waypoint to destination area | `waypoint_id`, `destination` (area ID) |
| `cast_skill` | Cast skill at location or on unit | `x`, `y`, `unit_id`, `unit_type`, `left` |
| `click_screen` | Simulate mouse click (needs foreground) | `x`, `y`, `button` |

## UI & Menu

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_controls` | Dump all UI controls with text content and addresses | |
| `click_control` | Click a control by index (calls OnPress, no foreground needed) | `index`, `mouse_flow` |
| `is_panel_open` | Check if stash/trade/waypoint panel is open | |
| `close_panels` | Close all open UI panels | |
| `wait_until` | Poll for condition: panel_open, position_reached, cursor_empty | `condition`, `timeout_ms` |

## Player State

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_player_state` | Full stats: HP, MP, resists, FCR, combat stats, position, XP% | |
| `get_skills` | All skill allocations, build detection (e.g., "Sorceress (Lightning)") | |
| `get_belt_contents` | Belt slot contents (potions) | |
| `get_nearby_units` | Monsters/players/items with immunities and resistances | `max_distance` |
| `get_nearby_objects` | Game objects: stash, waypoint, shrines, portals | `max_distance` |
| `get_waypoints` | Available waypoint destinations (panel must be open) | |

## Inventory & Items

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_inventory` | All items: inventory, belt, equipped, stash, cube | |
| `get_item_stats` | Full affixes on any item: quality, sockets, ethereal, all stats | `item_id` |
| `get_stash_grid` | Occupancy grid (unit_id per cell) | `container` |
| `get_cursor_item` | Check if item is on cursor | |
| `item_to_cursor` | Pick item to cursor | `item_id` |
| `cursor_to_container` | Place cursor item | `item_id`, `x`, `y`, `container` |
| `move_item` | Atomic pick + optional tab switch + place + verify | `item_id`, `dest_x`, `dest_y`, `dest_tab`, `dest_container` |
| `drop_item` | Drop cursor item to ground | `item_id` |
| `pickup_item` | Pick up ground item | `item_id` |
| `use_item` | Use item (drink potion, read scroll) | `item_id` |

## Stash

| Tool | Description | Key Args |
|------|-------------|----------|
| `open_stash` | Find stash + interact + verify opened | |
| `switch_stash_tab` | Switch tab 0-10 (Personal, Shared I-IX, Materials) | `tab` |

## Vendor & Trade

| Tool | Description | Key Args |
|------|-------------|----------|
| `sell_item` | Sell item to open NPC trade | `item_id`, `npc_id` |

## Auto Features

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_auto_potion` | Auto-potion config and status | |
| `set_auto_potion` | Configure HP/MP/Rejuv thresholds | `enabled`, `hp_threshold`, etc. |
| `get_auto_pickup` | Auto-pickup config | |
| `set_auto_pickup` | Configure belt refill | `enabled`, `pickup_distance` |

## Function Hooking

| Tool | Description | Key Args |
|------|-------------|----------|
| `install_hook` | Detours hook on game function | `address`, `name`, `capture_level` |
| `remove_hook` | Remove hook | `address` |
| `list_hooks` | All hooks with call counts | |
| `get_call_log` | Recent call log entries | `count` |

## Game Loop Control

| Tool | Description | Key Args |
|------|-------------|----------|
| `pause_game` | Freeze game loop (MCP stays responsive) | |
| `resume_game` | Resume after pause | |
| `step_game` | Advance one frame while paused | |

## Memory & Structs

| Tool | Description | Key Args |
|------|-------------|----------|
| `read_memory` | Read bytes from address | `address`, `size`, `format` |
| `write_memory` | Write bytes to address | `address`, `bytes` |
| `read_struct` | Read as typed struct | `address`, `struct_name` |
| `read_region` | Classify DWORDs (pointer/string/int) | `address`, `count` |
| `list_struct_defs` | All struct definitions | |
| `get_struct_def` | Struct definition by name | `name` |
| `save_struct_defs` | Save to structs.json | |

## Binary Patching

| Tool | Description | Key Args |
|------|-------------|----------|
| `apply_patch` | Apply named patch | `name`, `address`, `bytes` |
| `toggle_patch` | Toggle patch on/off | `name` |
| `list_patches` | All patches with status | |
| `import_patches` / `export_patches` | JSON patch import/export | |

## Memory Watches

| Tool | Description | Key Args |
|------|-------------|----------|
| `add_watch` | Monitor address for changes | `name`, `address`, `size` |
| `remove_watch` | Remove watch | `name` |
| `get_watches` | All watches with values | |

## Low-Level

| Tool | Description | Key Args |
|------|-------------|----------|
| `resolve_function` | DLL function address by ordinal/name | `dll`, `ordinal`, `name` |
| `call_function` | Call game function with args | `address`, `args`, `convention` |
| `get_crash_log` | Crash records with registers | |
| `ping` | Test connectivity | |
| `get_game_info` | Process info | |
