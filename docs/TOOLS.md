# MCP Tools Reference

## In-Game MCP Server (BH.dll, port 21337)

HTTP server at `localhost:21337`. All tools called via JSON-RPC 2.0 POST to `/mcp`.

```bash
curl -s http://localhost:21337/mcp -X POST -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"TOOL","arguments":{...}}}'
```

### Core (5)

| Tool | Description | Key Args |
|------|-------------|----------|
| `ping` | Test connectivity | |
| `get_game_info` | Process info, module bases | |
| `get_game_state` | State (in_game/menu), area, difficulty, frame | |
| `get_controls` | UI controls on current screen (buttons, textboxes) | |
| `capture_screen` | Screenshot game window, returns PNG image | |

### Player State (7)

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_player_state` | HP, mana, position, area, level, class, stats, resists, breakpoints | |
| `get_belt_contents` | Belt slot contents (potions, scrolls) | |
| `get_skills` | All skills with IDs, levels, tree, left/right assignment | |
| `get_inventory` | Items by location | `location` (equipped/inventory/cube/stash/belt) |
| `get_item_stats` | Full stats/affixes on an item | `unit_id` |
| `get_stash_grid` | Stash occupancy grid per tab | |
| `get_cursor_item` | Item currently on cursor | |

### World (7)

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_nearby_units` | Monsters, players, items with HP, distance, immunities | `max_distance` |
| `get_nearby_objects` | Waypoints, stash, shrines, chests, portals | `max_distance` |
| `get_level_exits` | Exits and waypoints for current level | |
| `get_waypoints` | All waypoints with unlock status, supports act tab switching | `act`, `act_tab` |
| `get_collision_map` | Walkable/unwalkable grid for current area | |
| `find_teleport_path` | A* path from current position to target | `target_x`, `target_y` |
| `reveal_map` | Reveal automap for current level | |

### Combat & Movement (7)

| Tool | Description | Key Args |
|------|-------------|----------|
| `cast_skill` | Cast current skill at location or on unit | `x`, `y`, `unit_id`, `unit_type`, `left` |
| `switch_skill` | Switch active skill with verification | `skill_id`, `left` |
| `attack_unit` | D2CLIENT_Attack — direct function call combat | `unit_id`, `unit_type` |
| `walk_to` | Walk/run to world coordinates | `x`, `y`, `run` |
| `interact_entity` | Walk to + interact + verify panel opened | `unit_id`, `unit_type`, `expected_panel` |
| `interact_object` | Send interact packet (no verification) | `unit_id`, `unit_type` |
| `pickup_item` | Pick up ground item | `unit_id` |

### Items (6)

| Tool | Description | Key Args |
|------|-------------|----------|
| `use_item` | Use item (drink potion, read scroll) | `unit_id` |
| `drop_item` | Drop cursor item to ground | |
| `move_item` | Move item between containers | `unit_id`, `target`, `x`, `y` |
| `item_to_cursor` | Pick up item to cursor | `unit_id` |
| `cursor_to_container` | Place cursor item in container | `x`, `y`, `container` |
| `sell_item` | Sell item to open NPC trade | `unit_id` |

### Automation (6)

| Tool | Description | Key Args |
|------|-------------|----------|
| `get_auto_potion` | Current auto-potion config and status | |
| `set_auto_potion` | Configure HP/MP/rejuv thresholds | `enabled`, `hp_threshold`, `mp_threshold`, `rejuv_threshold` |
| `get_auto_pickup` | Current auto-pickup config | |
| `set_auto_pickup` | Configure auto-pickup settings | `enabled`, `range`, `tp`, `id` |
| `get_auto_cast` | Current auto-cast config | |
| `set_auto_cast` | Configure auto-cast skills, range, priority | `enabled`, `skill_id`, `backup_skill_id`, `range`, `priority` |

### Navigation (11)

| Tool | Description | Key Args |
|------|-------------|----------|
| `enter_game` | Navigate menus to enter a game | `character`, `difficulty` |
| `exit_game` | Save and exit via D2CLIENT_ExitGame | |
| `quit_game` | Save and terminate Game.exe | |
| `select_character` | Select character on char select screen | `name` |
| `launch_character` | (Deprecated) Launch by internal index | `name` |
| `get_nav_status` | Menu navigation progress | |
| `use_waypoint` | Travel via waypoint | `waypoint_id`, `destination` |
| `close_panels` | Close all open UI panels | |
| `is_panel_open` | Check which panels are open | |
| `click_control` | Click UI control by index (calls OnPress) | `index` |
| `wait_until` | Poll for a condition | `condition`, `timeout_ms` |

### Stash (4)

| Tool | Description | Key Args |
|------|-------------|----------|
| `open_stash` | Find and open stash chest | |
| `open_cube` | Open Horadric Cube panel | |
| `switch_stash_tab` | Switch PD2 stash tab (0-10) | `tab` |
| `click_screen` | (Deprecated) Simulate mouse click | `x`, `y` |

### Memory & Debug (7)

| Tool | Description | Key Args |
|------|-------------|----------|
| `read_memory` | Read bytes from game memory | `address`, `size` |
| `write_memory` | Write bytes to game memory | `address`, `bytes` |
| `read_struct` | Read typed struct with field names | `address`, `type` |
| `read_region` | Classify unknown memory (pointer/string/int) | `address`, `size` |
| `add_watch` | Watch memory address for changes | `name`, `address`, `size` |
| `remove_watch` | Remove a watch | `name` |
| `get_watches` | All watches with current/previous values | |

### Functions (2)

| Tool | Description | Key Args |
|------|-------------|----------|
| `call_function` | Call game function on game thread | `address`, `args`, `convention` |
| `resolve_function` | Resolve DLL ordinal to address | `dll`, `ordinal` |

### Structs (3)

| Tool | Description | Key Args |
|------|-------------|----------|
| `list_struct_defs` | List all known struct definitions | |
| `get_struct_def` | Get a struct definition by name | `name` |
| `save_struct_defs` | Save struct defs to JSON file | |

### Hooking (4)

| Tool | Description | Key Args |
|------|-------------|----------|
| `install_hook` | Hook game function via Detours | `address`, `name` |
| `remove_hook` | Remove hook by address | `address` |
| `list_hooks` | All installed hooks with call counts | |
| `get_call_log` | Recent function call log entries | `count` |

### Patches (5)

| Tool | Description | Key Args |
|------|-------------|----------|
| `list_patches` | All managed patches with status | |
| `apply_patch` | Apply named binary patch | `name`, `address`, `bytes` |
| `toggle_patch` | Toggle patch on/off | `name` |
| `import_patches` | Import patches from JSON | `patches` |
| `export_patches` | Export patches to JSON file | |

### Game Control (3)

| Tool | Description | Key Args |
|------|-------------|----------|
| `pause_game` | Pause game loop (MCP stays responsive) | |
| `resume_game` | Resume game loop | |
| `step_game` | Advance one frame while paused | |
| `get_crash_log` | Captured crash/exception records | |

### Stream (2)

| Tool | Description | Key Args |
|------|-------------|----------|
| `update_stream` | Update stream overlay stats and messages | `status`, `event`, `message`, counters |
| `get_stream_stats` | Current stream stats as JSON | |

---

## Python Orchestrator (port 21338)

Persistent MCP server that survives game crashes.

| Tool | Description | Key Args |
|------|-------------|----------|
| `switch_character` | Exit → relaunch → select → enter game | `character`, `difficulty` |
| `new_game` | Exit → relaunch → same character | `difficulty` |
| `launch_game` | Deploy DLL and start Game.exe | `character`, `difficulty` |
| `get_status` | Game running? MCP alive? Character? Area? | |
| `proxy` | Forward tool call to in-game MCP | `tool`, `arguments` |
