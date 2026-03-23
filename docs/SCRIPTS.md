# Python Scripts Guide

All scripts are in `scripts/` and require `pip install requests`.

## game_manager.py — Game Lifecycle

Full autonomous cycle: kill old process → deploy DLL → launch game → navigate menus → enter game.

```bash
python game_manager.py                          # Full cycle, default character
python game_manager.py --character combustion   # Select specific character
python game_manager.py --kill                   # Kill Game.exe only
python game_manager.py --launch                 # Launch only
python game_manager.py --navigate               # Navigate to in-game only
python game_manager.py --status                 # Show current state
python game_manager.py --no-deploy              # Skip DLL copy
```

**Menu navigation flow:**
1. Main Menu → clicks Single Player
2. Gateway (PD2-specific) → cancels, retries Single Player
3. Character Select → finds character by name, clicks OK
4. Difficulty Select → clicks highest available
5. Loading → waits for in_game state

**Kill strategies** (tried in order): MCP quit_game, taskkill, PowerShell Stop-Process, WMI, elevated taskkill.

## farming_loop.py — Automated Farming

Teleports to farming area, kills monsters, picks up loot, returns to town, vendors junk.

```bash
python farming_loop.py --area 35 --runs 3 --duration 60    # Farm Catacombs, 3 runs
python farming_loop.py --area 35 --runs 1 --duration 30    # Quick test run
python farming_loop.py --dry-run --area 35                  # Show plan only
```

**Area IDs** (must be unlocked waypoint for current character):
- Act 1: 1=Rogue Encampment, 2=Cold Plains, 35=Catacombs Level 2
- Act 2: 40=Lut Gholein, 54=Arcane Sanctuary
- Act 3: 75=Kurast Docks, 83=Travincal
- Act 4: 103=Pandemonium Fortress, 107=River of Flame
- Act 5: 109=Harrogath, 129=Worldstone Keep Level 2

**Safety features:**
- Merc filter (won't attack mercenaries or town NPCs)
- Lightning immune skip (Sorc won't attack immune monsters)
- Chicken: exits game if HP < 20%
- Low HP retreat: teleports away if HP < 50%
- Stuck detection: checks mobility before each run
- Exploration limit: stops teleporting after 20 empty teleports

## stash_organizer.py — Stash Management

Scans all tabs, categorizes items, reorganizes across tabs.

```bash
python stash_organizer.py --scan                # Show current layout
python stash_organizer.py --plan                # Show what would move
python stash_organizer.py --simulate            # Fast simulation with action log
python stash_organizer.py --organize            # Execute moves
python stash_organizer.py --organize --max-moves 50   # Limit moves
python stash_organizer.py --defrag              # Repack all tabs
python stash_organizer.py --defrag-tab 5        # Repack specific tab
python stash_organizer.py --stash-inventory     # Move inventory items to stash
python stash_organizer.py --log-detail          # Print every action
python stash_organizer.py --log-file run.json   # Save action log
```

**Layout** (configurable in DEFAULT_LAYOUT):
- Tab 0: Personal (don't touch)
- Tab 1: Charms (1/2)
- Tab 2: Jewels & Gems
- Tab 3: Rings & Amulets
- Tab 4: Charms (2/2)
- Tab 5: Body Armor
- Tab 6: Boots, Gloves, Belts
- Tab 7: Helmets, Shields, Weapons
- Tab 8: PD2 Special & Tokens
- Tab 9: Overflow & Misc

**Strategy: D+B** (keep-in-place + sub-split)
- Items already on their correct tab stay put (50%+ threshold)
- Two-pass execution: clears space first, then places items
- Multi-tab categories: charms split across tabs 1+4

## auto_vendor.py — Auto-Vendoring

Evaluates items, moves to inventory, walks to NPC, sells.

```bash
python auto_vendor.py                           # Vendor inventory items
python auto_vendor.py --stash-tab 2             # Vendor from specific tab
python auto_vendor.py --all-stash               # Vendor across all tabs
python auto_vendor.py --dry-run                 # Preview what would sell
```

**Flow:** Open stash → scan tab → evaluate items → move vendorables to inventory → close stash → walk to NPC → open trade → sell each item.

**Protected items** (never sold): Tome of TP, Tome of ID, Horadric Cube, PD2 materials (Voidstone, Vision of Terror, Pandemonium Talisman, Skeleton Key, Puzzlebox, Worldstone Shard).

## item_evaluator.py — Item Evaluation

Scores items as keep/vendor/review based on configurable rules.

```bash
python item_evaluator.py --item 680             # Evaluate single item
python item_evaluator.py --scan-tab 1           # Evaluate a stash tab
python item_evaluator.py --scan-all             # Evaluate entire stash
python item_evaluator.py --vendor-list          # Show vendor candidates only
```

**Rules summary:**
- Small Charms: keep if life≥17, resist≥9, or good dual-mod
- Grand Charms: keep all skillers; vendor non-skillers unless life≥30
- Large Charms: generally vendor
- Equipment: auto-keep unique/set, auto-vendor inferior/normal
- Jewels: keep if IAS, ED+resist, or good procs
- Protected items: always keep regardless of stats
