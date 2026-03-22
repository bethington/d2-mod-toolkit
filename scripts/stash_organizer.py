"""Stash Organizer — Automated cross-tab inventory management.

Scans all stash tabs, categorizes items, and reorganizes them according
to a configurable layout. Uses the move_item MCP tool for reliable
atomic item movement with cursor verification.

Usage:
    python stash_organizer.py --scan              # Scan and show current layout
    python stash_organizer.py --plan              # Show proposed reorganization
    python stash_organizer.py --simulate          # Simulate moves, log efficiency
    python stash_organizer.py --organize          # Execute reorganization
    python stash_organizer.py --stash-inventory   # Move inventory items to stash
    python stash_organizer.py --defrag            # Repack tabs to eliminate gaps
    python stash_organizer.py --dry-run           # Show plan without moving

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


# ---- Action Log ----

@dataclass
class Action:
    """A single recorded action."""
    action: str          # "pick", "place", "tab_switch", "clear_cursor"
    item_id: int = 0
    item_name: str = ""
    from_tab: int = -1
    from_pos: Tuple[int, int] = (0, 0)
    to_tab: int = -1
    to_pos: Tuple[int, int] = (0, 0)
    result: str = ""     # "ok", "swap", "fail", "skip", "wasted"
    elapsed_ms: int = 0
    note: str = ""

class ActionLog:
    """Tracks all actions for efficiency analysis."""
    def __init__(self):
        self.actions: List[Action] = []
        self._start = time.time()

    def log(self, **kwargs) -> Action:
        a = Action(**kwargs)
        self.actions.append(a)
        return a

    def summary(self) -> dict:
        total = len(self.actions)
        moves = [a for a in self.actions if a.action in ("pick", "place")]
        tab_switches = [a for a in self.actions if a.action == "tab_switch"]
        wasted = [a for a in self.actions if a.result == "wasted"]
        swaps = [a for a in self.actions if a.result == "swap"]
        failures = [a for a in self.actions if a.result == "fail"]
        successful_moves = [a for a in self.actions if a.action == "move" and a.result == "ok"]
        elapsed = time.time() - self._start
        return {
            "total_actions": total,
            "successful_moves": len(successful_moves),
            "tab_switches": len(tab_switches),
            "swaps": len(swaps),
            "wasted": len(wasted),
            "failures": len(failures),
            "elapsed_sec": round(elapsed, 1),
            "moves_per_min": round(len(successful_moves) / max(elapsed / 60, 0.01), 1),
        }

    def print_summary(self):
        s = self.summary()
        print(f"\n=== ACTION LOG SUMMARY ===")
        print(f"  Successful moves:  {s['successful_moves']}")
        print(f"  Tab switches:      {s['tab_switches']}")
        print(f"  Swaps handled:     {s['swaps']}")
        print(f"  Wasted moves:      {s['wasted']}")
        print(f"  Failures:          {s['failures']}")
        print(f"  Total actions:     {s['total_actions']}")
        print(f"  Elapsed:           {s['elapsed_sec']}s")
        print(f"  Speed:             {s['moves_per_min']} moves/min")

    def print_detail(self, last_n=0):
        """Print detailed log. last_n=0 means all."""
        actions = self.actions[-last_n:] if last_n else self.actions
        for i, a in enumerate(actions):
            parts = [f"{a.action:15s}"]
            if a.item_name:
                parts.append(f"{a.item_name[:25]:25s}")
            if a.from_tab >= 0:
                parts.append(f"tab{a.from_tab}({a.from_pos[0]},{a.from_pos[1]})")
            if a.to_tab >= 0:
                parts.append(f"-> tab{a.to_tab}({a.to_pos[0]},{a.to_pos[1]})")
            parts.append(f"[{a.result}]")
            if a.note:
                parts.append(a.note)
            print(f"  {' '.join(parts)}")

    def save(self, path: str):
        """Save log to JSON file."""
        data = {
            "summary": self.summary(),
            "actions": [
                {k: v for k, v in a.__dict__.items() if v}
                for a in self.actions
            ]
        }
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
        print(f"  Log saved to {path}")


# ---- Protected Items ----

# Inventory rows 2-3 are locked (user's equipped gear)
LOCKED_INVENTORY_ROWS = {2, 3}

# Items to never move (by name substring, case-insensitive)
PROTECTED_ITEMS = [
    "tome of town portal",
    "tome of identify",
    "horadric cube",
]

def is_protected(item: dict) -> bool:
    """Check if an item should never be moved."""
    name = item.get("name", "").lower()
    # Remove color codes
    while "\xc3\xbf" in name or "ÿc" in name:
        idx = name.find("ÿc")
        if idx >= 0:
            name = name[:idx] + name[idx+3:]
        else:
            break
    return any(p in name for p in PROTECTED_ITEMS)

def is_in_locked_row(item: dict) -> bool:
    """Check if an inventory item is in a locked row."""
    return item.get("grid_y", 0) in LOCKED_INVENTORY_ROWS


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
        self.log = ActionLog()

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

    def _execute_single_move(self, m, dest_grids, idx, total, simulate=False) -> str:
        """Execute a single move. Returns 'ok', 'skip', or 'fail'."""
        item = m["item"]
        uid = item["unit_id"]
        name = m["name"][:30]
        from_tab, to_tab = m["from_tab"], m["to_tab"]
        from_pos = (item.get("grid_x", 0), item.get("grid_y", 0))
        sx, sy = m["size_x"], m["size_y"]

        dest = dest_grids.get(to_tab)
        if not dest:
            dest = Container(name=f"Tab {to_tab}", width=10, height=16, tab=to_tab)
            dest_grids[to_tab] = dest

        spot = dest.find_spot(sx, sy)
        if not spot:
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, result="skip", note="no space")
            if not simulate:
                print(f"  [{idx}/{total}] SKIP {name}: no space in tab {to_tab}")
            return "skip"

        dest_x, dest_y = spot

        # Check if this would be a wasted move (same position)
        if from_tab == to_tab and from_pos == (dest_x, dest_y):
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, to_pos=(dest_x, dest_y),
                         result="wasted", note="already at target")
            return "skip"

        if simulate:
            # Simulation: just update virtual grids, no real moves
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, to_pos=(dest_x, dest_y), result="ok")
            if from_tab in dest_grids:
                dest_grids[from_tab].unmark(item)
            dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            if from_tab != to_tab:
                self.log.log(action="tab_switch", from_tab=from_tab, to_tab=to_tab, result="ok")
            return "ok"

        if not self._clear_cursor():
            self.log.log(action="move", item_id=uid, item_name=name,
                         result="skip", note="cursor stuck")
            print(f"  [{idx}/{total}] SKIP {name}: cursor stuck")
            return "skip"

        print(f"  [{idx}/{total}] {name} (tab {from_tab} -> tab {to_tab} at {dest_x},{dest_y})...", end=" ", flush=True)

        t0 = time.time()
        result = self.mcp.call("move_item", {
            "item_id": uid, "dest_container": "stash",
            "dest_x": dest_x, "dest_y": dest_y, "dest_tab": to_tab,
        }, timeout=20)
        elapsed = int((time.time() - t0) * 1000)

        status = result.get("status", "unknown")
        if status == "moved":
            print("OK")
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, to_pos=(dest_x, dest_y),
                         result="ok", elapsed_ms=elapsed)
            if from_tab in dest_grids:
                dest_grids[from_tab].unmark(item)
            dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            return "ok"
        elif status == "swapped":
            swap = result.get("swapped_item", {})
            print(f"SWAP ({swap.get('name', '?')})")
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, to_pos=(dest_x, dest_y),
                         result="swap", elapsed_ms=elapsed,
                         note=f"displaced {swap.get('name', '?')}")
            if from_tab in dest_grids:
                dest_grids[from_tab].unmark(item)
            dest.reserve_spot(dest_x, dest_y, sx, sy, uid)
            self._clear_cursor()
            return "ok"
        else:
            print(f"FAIL ({status})")
            self.log.log(action="move", item_id=uid, item_name=name,
                         from_tab=from_tab, from_pos=from_pos,
                         to_tab=to_tab, to_pos=(dest_x, dest_y),
                         result="fail", elapsed_ms=elapsed, note=status)
            self._clear_cursor()
            return "fail"

    def execute_moves(self, moves, dry_run=False, simulate=False, max_moves=None):
        """Two-pass execution: clear space first, then place items."""
        if dry_run:
            self.show_plan(moves)
            print("\n[DRY RUN] No items moved.")
            return

        if not moves:
            print("Nothing to reorganize!")
            return

        self.show_plan(moves)
        self.log = ActionLog()  # fresh log

        # Split into two passes
        receiving_tabs = set(m["to_tab"] for m in moves)
        pass1 = [m for m in moves if m["from_tab"] in receiving_tabs]
        pass2 = [m for m in moves if m["from_tab"] not in receiving_tabs]
        all_ordered = pass1 + pass2
        total = len(all_ordered)

        mode = "SIMULATING" if simulate else "EXECUTING"
        print(f"\n=== {mode} {total} MOVES (pass1: {len(pass1)} clearing, pass2: {len(pass2)} placing) ===\n")

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
            result = self._execute_single_move(m, dest_grids, i + 1, total, simulate=simulate)
            if result == "ok":
                success += 1
            elif result == "fail":
                failed += 1
            else:
                skipped += 1

        print(f"\n=== DONE: {success} moved, {failed} failed, {skipped} skipped ===")
        self.log.print_summary()

    def organize(self, dry_run=False, simulate=False, max_moves=None, log_file=None):
        """Full reorganization pipeline."""
        moves = self.plan_reorganization()
        self.execute_moves(moves, dry_run=dry_run, simulate=simulate, max_moves=max_moves)
        if log_file:
            self.log.save(log_file)

    def stash_inventory(self, dry_run=False):
        """Move non-protected inventory items into the correct stash tabs.

        Respects:
        - Locked inventory rows (2-3): never touched
        - Protected items (cubes, tomes): never moved
        - Layout config: items go to their category's tab
        """
        inv = self.mcp.call("get_inventory")
        inv_items = [i for i in inv.get("items", []) if i.get("storage") == "inventory"]

        # Get item sizes from grid
        grid_data = self.mcp.call("get_stash_grid", {"container": "inventory"})
        grid = grid_data.get("grid", [])
        uid_cells = {}
        for y, row in enumerate(grid):
            for x, uid in enumerate(row):
                if uid:
                    uid_cells.setdefault(uid, set()).add((x, y))
        for item in inv_items:
            cells = uid_cells.get(item["unit_id"], set())
            if cells:
                xs = [c[0] for c in cells]
                ys = [c[1] for c in cells]
                item["size_x"] = max(xs) - min(xs) + 1
                item["size_y"] = max(ys) - min(ys) + 1
                item["grid_x"] = min(xs)
                item["grid_y"] = min(ys)
            else:
                item["size_x"] = item["size_y"] = 1

        # Filter: skip locked rows and protected items
        moveable = []
        for item in inv_items:
            if is_in_locked_row(item):
                continue
            if is_protected(item):
                continue
            moveable.append(item)

        if not moveable:
            print("No inventory items to stash.")
            return

        print(f"\n=== STASH INVENTORY: {len(moveable)} items to move ===\n")
        for item in moveable:
            cat = categorize_item(item["name"])
            target_tab = self._find_target_tab(cat)
            name = item["name"][:30]
            uid = item["unit_id"]
            sx, sy = item.get("size_x", 1), item.get("size_y", 1)

            if dry_run:
                print(f"  [DRY] {name} -> tab {target_tab} ({cat})")
                continue

            # Switch to target tab and find spot
            self.mcp.call("switch_stash_tab", {"tab": target_tab})
            time.sleep(0.5)
            stash_grid = self.mcp.call("get_stash_grid", {"container": "stash"})
            sg = stash_grid.get("grid", [])

            # Find spot that fits item size
            spot = None
            sh = len(sg)
            sw = len(sg[0]) if sg else 0
            for ty in range(sh - sy + 1):
                for tx in range(sw - sx + 1):
                    if all(sg[ty+dy][tx+dx] == 0 for dy in range(sy) for dx in range(sx)):
                        spot = (tx, ty)
                        break
                if spot:
                    break

            if not spot:
                print(f"  SKIP {name}: no space in tab {target_tab}")
                continue

            if not self._clear_cursor():
                print(f"  SKIP {name}: cursor stuck")
                continue

            # Pick from inventory
            self.mcp.call("item_to_cursor", {"item_id": uid})
            time.sleep(0.3)

            # Place in stash
            self.mcp.call("cursor_to_container", {
                "item_id": uid, "x": spot[0], "y": spot[1], "container": "stash"
            })
            time.sleep(0.3)

            cursor = self.mcp.call("get_cursor_item")
            if cursor.get("has_item"):
                print(f"  SWAP {name} -> tab {target_tab} at {spot}")
                self._clear_cursor()
            else:
                print(f"  OK   {name} -> tab {target_tab} at {spot}")

        if dry_run:
            print("\n[DRY RUN] No items moved.")

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
    parser.add_argument("--simulate", action="store_true", help="Simulate moves (fast, no real moves, logs efficiency)")
    parser.add_argument("--organize", action="store_true", help="Execute reorganization")
    parser.add_argument("--stash-inventory", action="store_true", help="Move inventory items to correct stash tabs")
    parser.add_argument("--defrag", action="store_true", help="Defrag tabs (repack items tightly)")
    parser.add_argument("--defrag-tab", type=int, help="Defrag a specific tab only")
    parser.add_argument("--dry-run", action="store_true", help="Show plan without moving")
    parser.add_argument("--max-moves", type=int, help="Limit number of moves")
    parser.add_argument("--log-file", type=str, help="Save action log to JSON file")
    parser.add_argument("--log-detail", action="store_true", help="Print detailed action log")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    organizer = StashOrganizer(args.url)

    if not organizer.mcp.alive():
        print("ERROR: MCP server not responding at", args.url)
        sys.exit(1)

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

    if args.stash_inventory:
        organizer.stash_inventory(dry_run=args.dry_run)
    elif is_defrag:
        tabs = [args.defrag_tab] if args.defrag_tab is not None else None
        organizer.defrag(tabs=tabs, dry_run=args.dry_run)
    elif args.simulate:
        organizer.organize(simulate=True, max_moves=args.max_moves,
                          log_file=args.log_file or "organize_sim.json")
    elif args.organize:
        organizer.organize(max_moves=args.max_moves, log_file=args.log_file)
    elif args.dry_run:
        organizer.organize(dry_run=True)

    if args.log_detail and organizer.log.actions:
        print("\n=== DETAILED ACTION LOG ===")
        organizer.log.print_detail()

    if not any([args.scan, args.plan, args.organize, args.dry_run,
                is_defrag, args.simulate, args.stash_inventory]):
        organizer.show_current()


if __name__ == "__main__":
    main()
