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

# Layout based on actual item census (543 items, 2026-03-22):
#   Charms: 168 (split across 2 tabs), Jewels: 107, Rings: 63,
#   Body Armor: 25, Amulets: 25, Belts: 17, Boots: 16, Helmets: 15,
#   Weapons: 22, Tokens/PD2: 25, Other: 42
DEFAULT_LAYOUT = {
    0: {"name": "Personal", "categories": []},       # Don't touch
    1: {"name": "Charms (1/2)", "categories": [       # ~76 charms already here
        "Grand Charm", "Large Charm", "Small Charm", "Charm"
    ]},
    2: {"name": "Jewels & Gems", "categories": [      # 107 jewels already here
        "Jewel", "Gem", "Rune"
    ]},
    3: {"name": "Rings & Amulets", "categories": [    # 63+25 = 88 already here
        "Ring", "Amulet"
    ]},
    4: {"name": "Charms (2/2)", "categories": [       # ~79 charms already here
        "Grand Charm", "Large Charm", "Small Charm", "Charm"
    ]},
    5: {"name": "Body Armor", "categories": [         # 25 armors (~150 cells)
        "Body Armor"
    ]},
    6: {"name": "Boots, Gloves, Belts", "categories": [ # 16+11+17 = 44 items
        "Boots", "Gloves", "Belt"
    ]},
    7: {"name": "Helmets, Shields, Weapons", "categories": [ # 15+6+22 = 43 items
        "Helmet", "Shield", "Sword", "Axe", "Mace",
        "Assassin Claw", "Spear/Javelin", "Polearm",
        "Caster Weapon", "Ranged Weapon"
    ]},
    8: {"name": "PD2 Special & Tokens", "categories": [ # 25 items
        "Token", "PD2 Special", "Key", "Quest"
    ]},
    9: {"name": "Overflow & Misc", "categories": [     # 42+ items
        "Potion", "Scroll/Tome", "Ear", "Other"
    ]},
}

# Keep-in-place threshold: if >= this % of a tab's items already match
# the target category, don't move those items (they're already home)
KEEP_IN_PLACE_THRESHOLD = 0.5  # 50%


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

    def _find_target_tab(self, category: str, current_tab: int = -1) -> int:
        """Find the best target tab for a category.

        If multiple tabs accept this category (e.g. charms on tabs 1+4),
        prefer the one with the most free space. If the item is already
        on one of the valid tabs, keep it there.
        """
        candidates = []
        for tab, cfg in self.layout.items():
            if tab == 0:
                continue
            if category in cfg.get("categories", []):
                candidates.append(tab)

        if not candidates:
            return 9  # overflow

        # If already on a valid tab, stay there
        if current_tab in candidates:
            return current_tab

        # Pick the candidate with the most free space
        if len(candidates) == 1:
            return candidates[0]

        best_tab = candidates[0]
        best_free = -1
        for tab in candidates:
            if tab in self.tabs:
                free = self.tabs[tab].free_cells()
                if free > best_free:
                    best_free = free
                    best_tab = tab
        return best_tab

    def _compute_keep_in_place(self) -> Dict[int, set]:
        """Determine which tabs already have enough matching items to keep.

        If >= KEEP_IN_PLACE_THRESHOLD of a tab's items belong to that tab's
        configured categories, mark those items as 'already home' and skip them.
        This prevents pointlessly moving 100 rings from tab 3 to tab 3.
        """
        keep_tabs = {}  # tab -> set of categories that should stay

        for tab, cfg in self.layout.items():
            if tab == 0 or not cfg.get("categories"):
                continue

            tab_items = [i for i in self.all_items if i["_tab"] == tab]
            if not tab_items:
                continue

            # Count items that BELONG on this tab vs items that should go elsewhere
            matching = sum(1 for i in tab_items if i["_category"] in cfg["categories"])
            ratio = matching / len(tab_items) if tab_items else 0

            if ratio >= KEEP_IN_PLACE_THRESHOLD:
                keep_tabs[tab] = set(cfg["categories"])

        return keep_tabs

    def plan_reorganization(self):
        """Plan item movements with keep-in-place optimization.

        Strategy D+B:
        - (B) Categories are sub-split across dedicated tabs in the layout
        - (D) If a tab already has 50%+ matching items, those items stay put
              Only items on the WRONG tab get moved
        """
        # Find which tabs are "already home" for their categories
        keep = self._compute_keep_in_place()

        moves = []
        kept_count = 0

        # Skip tab 0 (personal)
        moveable = [i for i in self.all_items if i["_tab"] != 0]

        for item in moveable:
            cat = item["_category"]
            current_tab = item["_tab"]
            target_tab = self._find_target_tab(cat, current_tab)

            # Keep-in-place check: if this item's category belongs on its
            # current tab AND that tab passes the threshold, skip it
            if current_tab in keep and cat in keep[current_tab]:
                kept_count += 1
                continue

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

        # Sort moves strategically:
        # 1. First move items OUT of tabs that need to receive items (free up space)
        # 2. Then move items INTO those tabs
        # This avoids circular dependencies where Tab A needs Tab B's space and vice versa
        receiving_tabs = set(m["to_tab"] for m in moves)
        def move_priority(m):
            # Items leaving a receiving tab get priority (frees space for incoming)
            is_clearing = 1 if m["from_tab"] in receiving_tabs else 0
            return (-is_clearing, m["from_tab"], m["to_tab"])
        moves.sort(key=move_priority)

        if kept_count > 0:
            print(f"  (Keeping {kept_count} items in place — already on correct tab)")

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
            print(f"-> Tab {tab} ({name}): {len(tab_moves)} items incoming")
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
            print(f"  Tab {tab}: {current} now, +{incoming} in, -{outgoing} out -> ~{final} items ({free} free cells)")

    def _clear_cursor(self) -> bool:
        """Ensure cursor is empty. Returns True if cursor is clear."""
        cursor = self.mcp.call("get_cursor_item")
        if not cursor.get("has_item"):
            return True
        cuid = cursor.get("unit_id", 0)
        for container in ["stash", "inventory"]:
            grid_data = self.mcp.call("get_stash_grid", {"container": container})
            grid = grid_data.get("grid", [])
            gh, gw = len(grid), (len(grid[0]) if grid else 0)
            for cy in range(gh):
                for cx in range(gw):
                    if grid[cy][cx] == 0:
                        self.mcp.call("cursor_to_container", {
                            "item_id": cuid, "x": cx, "y": cy,
                            "container": container,
                        })
                        time.sleep(0.3)
                        if not self.mcp.call("get_cursor_item").get("has_item"):
                            return True
        return False

    def _execute_single_move(self, m, dest_grids, idx, total) -> str:
        """Execute a single move. Returns 'ok', 'skip', or 'fail'."""
        item = m["item"]
        uid = item["unit_id"]
        from_tab, to_tab = m["from_tab"], m["to_tab"]
        sx, sy = m["size_x"], m["size_y"]

        dest = dest_grids.get(to_tab)
        if not dest:
            dest = Container(name=f"Tab {to_tab}", width=10, height=16, tab=to_tab)
            dest_grids[to_tab] = dest

        spot = dest.find_spot(sx, sy)
        if not spot:
            print(f"  [{idx}/{total}] SKIP {m['name']}: no space in tab {to_tab}")
            return "skip"

        if not self._clear_cursor():
            print(f"  [{idx}/{total}] SKIP {m['name']}: cursor stuck")
            return "skip"

        dest_x, dest_y = spot
        print(f"  [{idx}/{total}] {m['name']} (tab {from_tab} -> tab {to_tab} at {dest_x},{dest_y})...", end=" ", flush=True)

        result = self.mcp.call("move_item", {
            "item_id": uid, "dest_container": "stash",
            "dest_x": dest_x, "dest_y": dest_y, "dest_tab": to_tab,
        }, timeout=20)

        status = result.get("status", "unknown")
        if status == "moved":
            print("OK")
            if from_tab in dest_grids:
                dest_grids[from_tab].unmark(item)
            dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            return "ok"
        elif status == "swapped":
            swap = result.get("swapped_item", {})
            print(f"SWAP ({swap.get('name', '?')})")
            if from_tab in dest_grids:
                dest_grids[from_tab].unmark(item)
            dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            self._clear_cursor()
            return "ok"
        else:
            print(f"FAIL ({status})")
            self._clear_cursor()
            return "fail"

    def execute_moves(self, moves, dry_run=False, max_moves=None):
        """Two-pass execution: clear space first, then place items."""
        if dry_run:
            self.show_plan(moves)
            print("\n[DRY RUN] No items moved.")
            return

        if not moves:
            print("Nothing to reorganize!")
            return

        self.show_plan(moves)

        # Split into two passes:
        # Pass 1: Items leaving a tab that needs to RECEIVE items (frees space)
        # Pass 2: Everything else
        receiving_tabs = set(m["to_tab"] for m in moves)
        pass1 = [m for m in moves if m["from_tab"] in receiving_tabs]
        pass2 = [m for m in moves if m["from_tab"] not in receiving_tabs]

        all_ordered = pass1 + pass2
        total = len(all_ordered)

        print(f"\n=== EXECUTING {total} MOVES (pass1: {len(pass1)} clearing, pass2: {len(pass2)} placing) ===\n")

        dest_grids = {}
        for tab in self.tabs:
            dest_grids[tab] = Container(
                name=f"Plan {tab}", width=10, height=16, tab=tab,
                grid=[row[:] for row in self.tabs[tab].grid]
            )

        success = 0
        failed = 0
        skipped = 0
        move_count = 0

        for i, m in enumerate(all_ordered):
            if max_moves and move_count >= max_moves:
                print(f"\nStopped after {max_moves} attempted moves")
                break

            move_count += 1
            result = self._execute_single_move(m, dest_grids, i + 1, total)
            if result == "ok":
                success += 1
            elif result == "fail":
                failed += 1
            else:
                skipped += 1

        print(f"\n=== DONE: {success} moved, {failed} failed, {skipped} skipped ===")

    def organize(self, dry_run=False, max_moves=None):
        """Full reorganization pipeline."""
        moves = self.plan_reorganization()
        self.execute_moves(moves, dry_run=dry_run, max_moves=max_moves)

    def defrag_tab(self, tab: int, dry_run=False) -> int:
        """Defragment a single tab by repacking items top-left.

        Strategy: move items from the bottom/scattered positions into
        gaps at the top. Only moves items that would actually change
        position. Skips items already at their optimal spot.

        For full tabs: picks items from the bottom, temporarily holds
        on cursor, shifts items up to fill gaps, then places.
        """
        self.mcp.call("switch_stash_tab", {"tab": tab})
        time.sleep(0.5)

        items = self.scan_tab(tab)
        if not items:
            return 0

        # Sort items: largest first for better packing
        items.sort(key=lambda i: (i.get("size_x", 1) * i.get("size_y", 1)), reverse=True)

        # Compute optimal placement
        target = Container(name=f"Defrag {tab}", width=10, height=16, tab=tab)
        placements = []
        for item in items:
            sx, sy = item.get("size_x", 1), item.get("size_y", 1)
            spot = target.find_spot(sx, sy)
            if spot:
                target.reserve_spot(spot[0], spot[1], sx, sy, item["unit_id"])
                placements.append((item, spot[0], spot[1]))

        # Only move items that are NOT already at their target position
        moves = [(item, tx, ty) for item, tx, ty in placements
                 if item.get("grid_x", 0) != tx or item.get("grid_y", 0) != ty]

        if not moves:
            print(f"  Tab {tab}: already compact ({len(items)} items)")
            return 0

        print(f"  Tab {tab}: {len(moves)} of {len(items)} items need moving" +
              (" [DRY]" if dry_run else ""))

        if dry_run:
            return 0

        # Execute moves bottom-up: move items from highest Y first
        # (items at the bottom are moved into gaps at the top)
        moves.sort(key=lambda m: (-m[0].get("grid_y", 0), -m[0].get("grid_x", 0)))

        moved = 0
        for item, tx, ty in moves:
            uid = item["unit_id"]

            if not self._clear_cursor():
                continue

            result = self.mcp.call("move_item", {
                "item_id": uid, "dest_container": "stash",
                "dest_x": tx, "dest_y": ty,
            }, timeout=10)

            status = result.get("status", "unknown")
            if status == "moved":
                moved += 1
            elif status == "swapped":
                # Swap means target was occupied — item sizes didn't match plan.
                # Clear cursor and continue (the swap displaced another item
                # which may get moved in a later iteration)
                self._clear_cursor()
                moved += 1
            else:
                self._clear_cursor()

        return moved

    def defrag(self, tabs=None, dry_run=False):
        """Defragment stash tabs to eliminate gaps.

        Args:
            tabs: list of tab indices to defrag, or None for all (1-9)
            dry_run: if True, just show what would happen
        """
        if tabs is None:
            tabs = list(range(1, 10))  # skip personal

        print(f"\n=== DEFRAG {'(DRY RUN) ' if dry_run else ''}===\n")
        total_moved = 0
        for tab in tabs:
            moved = self.defrag_tab(tab, dry_run=dry_run)
            total_moved += moved

        if not dry_run:
            print(f"\n=== DEFRAG DONE: {total_moved} items repacked ===")
        else:
            print(f"\n=== DEFRAG DRY RUN: would repack items across {len(tabs)} tabs ===")
        return total_moved


# ---- CLI ----

def main():
    parser = argparse.ArgumentParser(description="Stash Organizer")
    parser.add_argument("--scan", action="store_true", help="Scan all tabs and show layout")
    parser.add_argument("--plan", action="store_true", help="Show reorganization plan")
    parser.add_argument("--organize", action="store_true", help="Execute reorganization")
    parser.add_argument("--defrag", action="store_true", help="Defrag tabs (repack items tightly)")
    parser.add_argument("--defrag-tab", type=int, help="Defrag a specific tab only")
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

    is_defrag = args.defrag or args.defrag_tab is not None

    if not is_defrag and (args.plan or args.dry_run):
        moves = organizer.plan_reorganization()
        organizer.show_plan(moves)

    if is_defrag:
        tabs = [args.defrag_tab] if args.defrag_tab is not None else None
        organizer.defrag(tabs=tabs, dry_run=args.dry_run)
    elif args.organize:
        organizer.organize(max_moves=args.max_moves)
    elif args.dry_run:
        organizer.organize(dry_run=True)

    if not any([args.scan, args.plan, args.organize, args.dry_run, is_defrag]):
        organizer.show_current()


if __name__ == "__main__":
    main()
