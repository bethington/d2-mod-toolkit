"""Stash Organizer — Automated cross-tab inventory management.

Scans all stash tabs, categorizes items, and reorganizes them according
to a configurable layout. Uses the move_item MCP tool for reliable
atomic item movement with cursor verification.

Usage:
    python stash_organizer.py --scan          # Scan and show current layout
    python stash_organizer.py --plan          # Show proposed reorganization
    python stash_organizer.py --organize      # Execute reorganization
    python stash_organizer.py --dry-run       # Show what would happen

Requires: game running with stash open, MCP server on port 21337.
"""

import json
import sys
import time
import argparse
from typing import Optional, Dict, List, Tuple
from dataclasses import dataclass, field

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


# ---- MCP Client ----

class McpClient:
    def __init__(self, url="http://127.0.0.1:21337"):
        self.url = url
        self._id = 0

    def call(self, tool, args=None, timeout=15):
        self._id += 1
        r = requests.post(f"{self.url}/mcp", json={
            "jsonrpc": "2.0", "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}
        }, timeout=timeout)
        result = r.json()
        text = ""
        is_error = False
        if "result" in result:
            res = result["result"]
            is_error = res.get("isError", False)
            if "content" in res:
                text = res["content"][0].get("text", "")
        parsed = {}
        if text.startswith(("{", "[")):
            parsed = json.loads(text)
        parsed["_is_error"] = is_error
        return parsed

    def alive(self):
        try:
            return requests.get(f"{self.url}/health", timeout=2).status_code == 200
        except:
            return False


# ---- Item Categorization ----

def categorize_item(name: str) -> str:
    """Categorize an item by its name into a storage category."""
    n = name.lower()
    # Remove D2 color codes (ÿcX)
    while "ÿc" in n:
        idx = n.find("ÿc")
        n = n[:idx] + n[idx+3:]

    if "charm" in n:
        if "grand" in n: return "Grand Charm"
        if "large" in n: return "Large Charm"
        if "small" in n: return "Small Charm"
        return "Charm"

    if "ring" in n: return "Ring"
    if "amulet" in n: return "Amulet"
    if "jewel" in n: return "Jewel"

    if any(x in n for x in ["ruby", "sapphire", "emerald", "diamond",
                             "topaz", "amethyst", "skull"]): return "Gem"
    if "rune" in n: return "Rune"

    if any(x in n for x in ["potion", "elixir"]): return "Potion"
    if any(x in n for x in ["scroll", "tome"]): return "Scroll/Tome"
    if "key" in n and "skeleton" not in n: return "Key"

    if "token" in n: return "Token"
    if any(x in n for x in ["voidstone", "vision of terror", "pandemonium",
                             "talisman", "bone fragment"]): return "PD2 Special"
    if "ear" in n: return "Ear"

    if any(x in n for x in ["boot", "greave"]): return "Boots"
    if any(x in n for x in ["glove", "gaunt", "fist", "vambrace"]): return "Gloves"
    if any(x in n for x in ["belt", "sash"]): return "Belt"
    if any(x in n for x in ["armor", "plate", "mail", "robe", "coat",
                             "breast", "shroud", "sacred"]): return "Body Armor"
    if any(x in n for x in ["helm", "crown", "mask", "diadem", "visage",
                             "cap", "casque", "armet", "bone"]): return "Helmet"
    if any(x in n for x in ["shield", "pavise", "monarch", "ward",
                             "buckler", "defender"]): return "Shield"

    if any(x in n for x in ["spear", "pike", "pilum", "lance",
                             "javelin", "mancatcher", "trident"]): return "Spear/Javelin"
    if any(x in n for x in ["staff", "wand", "orb", "scepter"]): return "Caster Weapon"
    if any(x in n for x in ["bow", "crossbow"]): return "Ranged Weapon"
    if any(x in n for x in ["sword", "blade", "scimitar", "crystal"]): return "Sword"
    if any(x in n for x in ["axe", "hatchet", "cleaver", "tomahawk"]): return "Axe"
    if any(x in n for x in ["mace", "club", "hammer", "flail",
                             "star", "scepter"]): return "Mace"
    if any(x in n for x in ["claw", "katar", "scissors", "suwayyah"]): return "Assassin Claw"
    if any(x in n for x in ["polearm", "scythe", "bardiche", "voulge",
                             "halberd", "thresher"]): return "Polearm"

    if any(x in n for x in ["cube"]): return "Quest"
    if any(x in n for x in ["inifuss", "khalim", "horadric"]): return "Quest"
    if "skeleton key" in n: return "PD2 Special"

    return "Other"


# ---- Container (kolbot-inspired) ----

@dataclass
class Container:
    """Grid-based container for tracking item positions."""
    name: str
    width: int
    height: int
    tab: int
    grid: List[List[int]] = field(default_factory=list)
    items: List[dict] = field(default_factory=list)

    def __post_init__(self):
        if not self.grid:
            self.grid = [[0] * self.width for _ in range(self.height)]

    def mark(self, item: dict) -> bool:
        """Mark an item's cells in the grid."""
        x, y = item.get("grid_x", 0), item.get("grid_y", 0)
        w, h = item.get("size_x", 1), item.get("size_y", 1)
        uid = item.get("unit_id", 1)
        for iy in range(y, min(y + h, self.height)):
            for ix in range(x, min(x + w, self.width)):
                self.grid[iy][ix] = uid
        self.items.append(item)
        return True

    def unmark(self, item: dict):
        """Remove an item from the grid."""
        uid = item.get("unit_id")
        for y in range(self.height):
            for x in range(self.width):
                if self.grid[y][x] == uid:
                    self.grid[y][x] = 0

    def find_spot(self, size_x: int, size_y: int) -> Optional[Tuple[int, int]]:
        """Find first available spot for an item of given size."""
        for y in range(self.height - size_y + 1):
            for x in range(self.width - size_x + 1):
                if all(self.grid[y + dy][x + dx] == 0
                       for dy in range(size_y) for dx in range(size_x)):
                    return (x, y)
        return None

    def reserve_spot(self, x: int, y: int, w: int, h: int, uid: int):
        """Reserve cells for a planned placement."""
        for dy in range(h):
            for dx in range(w):
                if y + dy < self.height and x + dx < self.width:
                    self.grid[y + dy][x + dx] = uid

    def free_cells(self) -> int:
        return sum(1 for row in self.grid for cell in row if cell == 0)

    def dump(self):
        print(f"{self.name} ({self.width}x{self.height}) — {self.free_cells()}/{self.width*self.height} free")
        for y, row in enumerate(self.grid):
            cells = ["." if c == 0 else "X" for c in row]
            print(f"  row {y:>2}: [{''.join(cells)}]")


# ---- Tab Layout Configuration ----

DEFAULT_LAYOUT = {
    0: {"name": "Personal (Sorc)", "categories": []},  # Don't touch personal
    1: {"name": "Jewelry & Charms", "categories": [
        "Ring", "Amulet", "Grand Charm", "Large Charm", "Small Charm", "Charm"
    ]},
    2: {"name": "Gems, Jewels, Runes", "categories": [
        "Gem", "Jewel", "Rune"
    ]},
    3: {"name": "Gloves & Boots", "categories": ["Gloves", "Boots"]},
    4: {"name": "Chest Armor & Belts", "categories": ["Body Armor", "Belt"]},
    5: {"name": "Helmets & Shields", "categories": ["Helmet", "Shield"]},
    6: {"name": "Weapons: Spears/Polearms", "categories": [
        "Spear/Javelin", "Polearm"
    ]},
    7: {"name": "Weapons: Swords/Axes/Maces", "categories": [
        "Sword", "Axe", "Mace", "Assassin Claw"
    ]},
    8: {"name": "Caster & Ranged", "categories": [
        "Caster Weapon", "Ranged Weapon"
    ]},
    9: {"name": "Special & Overflow", "categories": [
        "Token", "PD2 Special", "Potion", "Scroll/Tome", "Key", "Ear",
        "Quest", "Other"
    ]},
}


# ---- Organizer ----

class StashOrganizer:
    def __init__(self, mcp_url="http://127.0.0.1:21337", layout=None):
        self.mcp = McpClient(mcp_url)
        self.layout = layout or DEFAULT_LAYOUT
        self.tabs = {}       # tab -> Container
        self.all_items = []  # flat list of all items across tabs

    def scan_tab(self, tab: int) -> List[dict]:
        """Scan a single tab and return its items with sizes."""
        grid_data = self.mcp.call("get_stash_grid", {"container": "stash"})
        inv = self.mcp.call("get_inventory")

        items = [i for i in inv.get("items", []) if i.get("storage") == "stash"]

        # Build size map from grid (deduce item sizes from grid occupancy)
        grid = grid_data.get("grid", [])
        uid_cells = {}  # uid -> set of (x,y)
        for y, row in enumerate(grid):
            for x, uid in enumerate(row):
                if uid != 0:
                    uid_cells.setdefault(uid, set()).add((x, y))

        for item in items:
            uid = item["unit_id"]
            cells = uid_cells.get(uid, set())
            if cells:
                xs = [c[0] for c in cells]
                ys = [c[1] for c in cells]
                item["size_x"] = max(xs) - min(xs) + 1
                item["size_y"] = max(ys) - min(ys) + 1
            else:
                item["size_x"] = 1
                item["size_y"] = 1

        return items

    def scan_all_tabs(self):
        """Scan all stash tabs and catalog items."""
        tab_names = ["Personal"] + [f"Shared {i}" for i in range(1, 10)] + ["Materials"]
        self.all_items = []

        for tab in range(10):  # 0-9 (skip Materials for organizing)
            print(f"Scanning tab {tab} ({tab_names[tab]})...", end=" ", flush=True)
            result = self.mcp.call("switch_stash_tab", {"tab": tab})
            if result.get("_is_error"):
                print("SKIP (switch failed)")
                continue
            time.sleep(0.5)

            items = self.scan_tab(tab)
            for item in items:
                item["_tab"] = tab
                item["_category"] = categorize_item(item["name"])
                self.all_items.append(item)

            container = Container(name=tab_names[tab], width=10, height=16, tab=tab)
            for item in items:
                container.mark(item)
            self.tabs[tab] = container

            print(f"{len(items)} items, {container.free_cells()} free")

        self.mcp.call("switch_stash_tab", {"tab": 0})
        print(f"\nTotal: {len(self.all_items)} items across {len(self.tabs)} tabs")

    def show_current(self):
        """Display current layout summary."""
        print("\n=== CURRENT STASH LAYOUT ===\n")
        for tab in sorted(self.tabs.keys()):
            container = self.tabs[tab]
            items = [i for i in self.all_items if i["_tab"] == tab]
            cats = {}
            for i in items:
                cats[i["_category"]] = cats.get(i["_category"], 0) + 1

            print(f"Tab {tab} ({container.name}): {len(items)} items, {container.free_cells()} free cells")
            for cat in sorted(cats.keys()):
                print(f"  {cat}: {cats[cat]}")
            print()

    def plan_reorganization(self):
        """Plan item movements based on layout configuration."""
        moves = []

        # Skip tab 0 (personal)
        moveable = [i for i in self.all_items if i["_tab"] != 0]

        for item in moveable:
            cat = item["_category"]
            current_tab = item["_tab"]

            # Find target tab for this category
            target_tab = None
            for tab, cfg in self.layout.items():
                if tab == 0:
                    continue
                if cat in cfg.get("categories", []):
                    target_tab = tab
                    break
            if target_tab is None:
                target_tab = 9  # overflow

            if target_tab != current_tab:
                moves.append({
                    "item": item,
                    "from_tab": current_tab,
                    "to_tab": target_tab,
                    "name": item["name"],
                    "category": cat,
                    "size_x": item.get("size_x", 1),
                    "size_y": item.get("size_y", 1),
                })

        # Sort moves to minimize tab switches: group by (from_tab, to_tab)
        moves.sort(key=lambda m: (m["from_tab"], m["to_tab"]))
        return moves

    def show_plan(self, moves):
        """Display the reorganization plan."""
        print(f"\n=== REORGANIZATION PLAN ({len(moves)} moves) ===\n")

        by_dest = {}
        for m in moves:
            by_dest.setdefault(m["to_tab"], []).append(m)

        for tab in sorted(by_dest.keys()):
            tab_moves = by_dest[tab]
            name = self.layout.get(tab, {}).get("name", f"Tab {tab}")
            print(f"→ Tab {tab} ({name}): {len(tab_moves)} items incoming")
            cats = {}
            for m in tab_moves:
                cats[m["category"]] = cats.get(m["category"], 0) + 1
            for cat in sorted(cats.keys()):
                print(f"    {cat}: {cats[cat]}")
            print()

        # Space check
        print("=== SPACE CHECK ===")
        for tab in sorted(by_dest.keys()):
            current = len([i for i in self.all_items if i["_tab"] == tab])
            incoming = len(by_dest[tab])
            outgoing = len([m for m in moves if m["from_tab"] == tab])
            final = current + incoming - outgoing
            free = self.tabs[tab].free_cells() if tab in self.tabs else 160
            print(f"  Tab {tab}: {current} now, +{incoming} in, -{outgoing} out → ~{final} items ({free} free cells)")

    def execute_moves(self, moves, dry_run=False, max_moves=None):
        """Execute moves using move_item MCP tool with batched tab switching."""
        if dry_run:
            self.show_plan(moves)
            print("\n[DRY RUN] No items moved.")
            return

        if not moves:
            print("Nothing to reorganize!")
            return

        self.show_plan(moves)
        print(f"\n=== EXECUTING {len(moves)} MOVES ===\n")

        # Pre-compute target positions for each destination tab
        dest_grids = {}  # tab -> Container (virtual grid for placement planning)
        for tab in self.tabs:
            dest_grids[tab] = Container(
                name=f"Plan {tab}", width=10, height=16, tab=tab,
                grid=[row[:] for row in self.tabs[tab].grid]  # deep copy
            )

        success = 0
        failed = 0
        skipped = 0

        for i, m in enumerate(moves):
            if max_moves and i >= max_moves:
                print(f"\nStopped after {max_moves} moves (limit reached)")
                break

            item = m["item"]
            uid = item["unit_id"]
            from_tab = m["from_tab"]
            to_tab = m["to_tab"]
            sx, sy = m["size_x"], m["size_y"]

            # Find a free spot in the destination grid
            dest = dest_grids.get(to_tab)
            if not dest:
                dest = Container(name=f"Tab {to_tab}", width=10, height=16, tab=to_tab)
                dest_grids[to_tab] = dest

            spot = dest.find_spot(sx, sy)
            if not spot:
                print(f"  [{i+1}/{len(moves)}] SKIP {m['name']}: no space in tab {to_tab}")
                skipped += 1
                continue

            dest_x, dest_y = spot
            print(f"  [{i+1}/{len(moves)}] {m['name']} (tab {from_tab} → tab {to_tab} at {dest_x},{dest_y})...", end=" ", flush=True)

            # Use move_item for atomic pick+switch+place
            result = self.mcp.call("move_item", {
                "item_id": uid,
                "dest_container": "stash",
                "dest_x": dest_x,
                "dest_y": dest_y,
                "dest_tab": to_tab,
            }, timeout=20)

            status = result.get("status", "unknown")
            if status == "moved":
                print("OK")
                success += 1
                # Update virtual grids
                if from_tab in dest_grids:
                    dest_grids[from_tab].unmark(item)
                dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            elif status == "swapped":
                swap = result.get("swapped_item", {})
                print(f"SWAP (displaced: {swap.get('name', '?')})")
                # Item placed but something came to cursor — need to put it back
                self.mcp.call("cursor_to_container", {
                    "item_id": swap.get("unit_id", 0),
                    "x": item.get("grid_x", 0),
                    "y": item.get("grid_y", 0),
                    "container": "stash",
                })
                time.sleep(0.3)
                success += 1
                if from_tab in dest_grids:
                    dest_grids[from_tab].unmark(item)
                dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            else:
                print(f"FAIL ({status})")
                failed += 1
                # If cursor has item, try to return it
                cursor = self.mcp.call("get_cursor_item")
                if cursor.get("has_item"):
                    print(f"    Returning item to source tab {from_tab}...")
                    self.mcp.call("switch_stash_tab", {"tab": from_tab})
                    time.sleep(0.5)
                    self.mcp.call("cursor_to_container", {
                        "item_id": cursor.get("unit_id", uid),
                        "x": item.get("grid_x", 0),
                        "y": item.get("grid_y", 0),
                        "container": "stash",
                    })
                    time.sleep(0.3)

        print(f"\n=== DONE: {success} moved, {failed} failed, {skipped} skipped ===")

    def organize(self, dry_run=False, max_moves=None):
        """Full reorganization pipeline."""
        moves = self.plan_reorganization()
        self.execute_moves(moves, dry_run=dry_run, max_moves=max_moves)


# ---- CLI ----

def main():
    parser = argparse.ArgumentParser(description="Stash Organizer")
    parser.add_argument("--scan", action="store_true", help="Scan all tabs and show layout")
    parser.add_argument("--plan", action="store_true", help="Show reorganization plan")
    parser.add_argument("--organize", action="store_true", help="Execute reorganization")
    parser.add_argument("--dry-run", action="store_true", help="Show plan without moving")
    parser.add_argument("--max-moves", type=int, help="Limit number of moves")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    organizer = StashOrganizer(args.url)

    if not organizer.mcp.alive():
        print("ERROR: MCP server not responding at", args.url)
        sys.exit(1)

    # Check game state
    state = organizer.mcp.call("get_game_state")
    if state.get("state") != "in_game":
        print(f"ERROR: Not in game (state: {state.get('state', 'unknown')})")
        sys.exit(1)

    print("Scanning all stash tabs...")
    organizer.scan_all_tabs()

    if args.scan:
        organizer.show_current()

    if args.plan or args.dry_run:
        moves = organizer.plan_reorganization()
        organizer.show_plan(moves)

    if args.organize:
        organizer.organize(max_moves=args.max_moves)
    elif args.dry_run:
        organizer.organize(dry_run=True)

    if not any([args.scan, args.plan, args.organize, args.dry_run]):
        organizer.show_current()


if __name__ == "__main__":
    main()
