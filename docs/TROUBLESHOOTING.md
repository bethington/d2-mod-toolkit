# Troubleshooting

## Common Issues

### Game won't launch from scripts
**Symptom:** `game_manager.py` launches Game.exe but it exits immediately.
**Fix:** Must launch from the game directory. The script uses `cwd=GAME_DIR` but if called from a different process, the working directory may be wrong.
```bash
cd C:\Diablo2\ProjectD2_dlls_removed && Game.exe -3dfx
```

### MCP server not responding
**Symptom:** `curl localhost:21337` returns connection refused.
**Causes:**
1. Game not running → launch it
2. BH.dll not loaded → check game directory has BH.dll
3. MCP crashed during exit_game → restart game (known issue)
4. Port 21337 in use by old process → kill all Game.exe instances

### Character won't move (stuck bug)
**Symptom:** `walk_to` returns success but position doesn't change.
**Causes:**
1. Panel is open (stash/trade blocking input) → `close_panels`
2. Game in interaction state → `close_panels` then retry
3. Persistent stuck state → restart game via `game_manager.py`
**Prevention:** Always call `close_panels` before movement sequences.

### Stash tab switch not working
**Symptom:** `switch_stash_tab` returns "switched" but items don't change.
**Cause:** Stash panel not actually open. The tab handlers check `*DAT_10410688 == 0xC` and silently return if stash is closed.
**Fix:** Use `interact_entity` to open stash first, verify `is_panel_open` returns `stash_open: true`.

### Waypoint travel fails
**Symptom:** `use_waypoint` returns success but area doesn't change.
**Causes:**
1. Destination not unlocked → use `get_waypoints` to check available destinations
2. Not close enough to waypoint → walk within 3 units first
3. Panel blocking interaction → `close_panels` first
**Note:** Waypoint panel state uses `g_dwData_add0` at 0x6FBAADD0, NOT the stash state variable.

### Auto-vendor sells protected items
**Symptom:** Tomes, cube, or PD2 materials get vendored.
**Fix:** Update `PROTECTED_ITEMS` list in `item_evaluator.py`. Current protected items: Tome of TP, Tome of ID, Horadric Cube, all PD2 uber materials, Skeleton Key, Puzzlebox, Worldstone Shard.

### Can't kill Game.exe
**Symptom:** `taskkill /F` returns "Access denied".
**Cause:** Game runs with different privileges or PD2 anti-tamper.
**Fix:** Kill from Task Manager manually. The game_manager tries 6 kill strategies including elevated PowerShell.

### MCP crashes on exit_game
**Symptom:** MCP stops responding after `exit_game` is called.
**Cause:** Known issue — the MCP server thread doesn't survive the game→menu transition. Some D2Client globals get invalidated.
**Workaround:** Use `quit_game` (full process exit) + `game_manager.py` to restart instead of `exit_game`.

## Performance Tips

- **Stash scanning:** Each tab switch + grid read takes ~500ms. A full 10-tab scan takes ~5-7 seconds.
- **Item movement:** Each move_item takes ~400ms (same tab) or ~700ms (cross-tab with switch).
- **Farming:** Teleport + cast cycle is ~300ms per action. Exploration covers ~20 units per teleport.
- **Auto-vendor:** Batch size limited by inventory space (10×4 = 40 cells). Large items may need multiple vendor trips.

## Debug Tools

- `get_controls` — see all UI controls with text content (useful for unknown screens)
- `is_panel_open` — check stash/trade/waypoint state
- `read_memory` — read raw game memory at any address
- `get_crash_log` — view crash records if game crashed
- `install_hook` — hook game functions to log calls
- `--log-detail` flag on stash_organizer — see every action taken
