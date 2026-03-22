"""Stash Organizer — Automated cross-tab inventory management.

Scans all stash tabs, categorizes items, and reorganizes them according
to a configurable layout. Uses kolbot-inspired Container pattern for
grid management and safe item movement.

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

    def call(self, tool, args=None):
        self._id += 1
        r = requests.post(f"{self.url}/mcp", json={
            "jsonrpc": "2.0", "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}
        }, timeout=15)
        result = r.json()
        if "result" in result and "content" in result["result"]:
            text = result["result"]["content"][0].get("text", "")
            if text.startswith(("{", "[")):
                return json.loads(text)
        return {}

    def alive(self):
        try:
            return requests.get(f"{self.url}/health", timeout=2).status_code == 200
        except:
            return False


# ---- Item Categorization ----

def categorize_item(name: str) -> str:
    """Categorize an item by its name into a storage category."""
    n = name.lower()

    # Remove color codes
    while "\xc3\xbf" in n or "ÿc" in n:
        idx = n.find("ÿc")
        if idx >= 0:
            n = n[:idx] + n[idx+3:]  # skip ÿcX
        else:
            break

    if "charm" in n:
        if "grand" in n: return "Grand Charm"
        if "large" in n: return "Large Charm"
        if "small" in n: return "Small Charm"
        return "Charm"

    if any(x in n for x in ["ring"]): return "Ring"
    if any(x in n for x in ["amulet"]): return "Amulet"
    if any(x in n for x in ["jewel"]): return "Jewel"

    if any(x in n for x in ["ruby", "sapphire", "emerald", "diamond",
                             "topaz", "amethyst", "skull"]): return "Gem"
    if "rune" in n: return "Rune"

    if any(x in n for x in ["potion", "elixir"]): return "Potion"
    if any(x in n for x in ["scroll", "tome"]): return "Scroll/Tome"
    if "key" in n and "skeleton" not in n: return "Key"

    if any(x in n for x in ["token"]): return "Token"
    if any(x in n for x in ["voidstone", "vision of terror", "pandemonium",
                             "talisman", "bone fragment"]): return "PD2 Special"
    if "ear" in n: return "Ear"

    # Equipment
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
    if any(x in n for x in ["skeleton key"]): return "PD2 Special"

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

    def find_spot(self, size_x: int, size_y: int) -> Optional[Tuple[int, int]]:
        """Find first available spot for an item of given size."""
        for y in range(self.height - size_y + 1):
            for x in range(self.width - size_x + 1):
                fits = True
                for dy in range(size_y):
                    for dx in range(size_x):
                        if self.grid[y + dy][x + dx] != 0:
                            fits = False
                            break
                    if not fits:
                        break
                if fits:
                    return (x, y)
        return None

    def free_cells(self) -> int:
        return sum(1 for row in self.grid for cell in row if cell == 0)

    def dump(self):
        print(f"{self.name} ({self.width}x{self.height}) — {self.free_cells()}/{self.width*self.height} free")
        for y, row in enumerate(self.grid):
            cells = ["." if c == 0 else "X" for c in row]
            print(f"  row {y:>2}: [{''.join(cells)}]")


# ---- Tab Layout Configuration ----

# Default layout — can be customized
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
        self.tabs = {}  # tab -> Container
        self.all_items = []  # flat list of all items across tabs

    def scan_all_tabs(self):
        """Scan all 10 stash tabs and catalog items."""
        tab_names = ["Personal"] + [f"Shared {i}" for i in range(1, 10)]
        self.all_items = []

        for tab in range(10):
            print(f"Scanning tab {tab} ({tab_names[tab]})...", end=" ", flush=True)
            self.mcp.call("switch_stash_tab", {"tab": tab})
            time.sleep(2)

            inv = self.mcp.call("get_inventory", {"location": "all"})
            items = [i for i in inv.get("items", []) if i.get("storage") == "stash"]

            for item in items:
                item["_tab"] = tab
                item["_category"] = categorize_item(item["name"])
                self.all_items.append(item)

            container = Container(
                name=tab_names[tab], width=10, height=16, tab=tab
            )
            for item in items:
                container.mark(item)
            self.tabs[tab] = container

            print(f"{len(items)} items")

        # Switch back to personal
        self.mcp.call("switch_stash_tab", {"tab": 0})
        time.sleep(1)

        print(f"\nTotal: {len(self.all_items)} items")

    def show_current(self):
        """Display current layout summary."""
        print("\n=== CURRENT STASH LAYOUT ===\n")
        for tab in range(10):
            if tab not in self.tabs:
                continue
            container = self.tabs[tab]
            items = [i for i in self.all_items if i["_tab"] == tab]
            cats = {}
            for i in items:
                cat = i["_category"]
                cats[cat] = cats.get(cat, 0) + 1

            print(f"Tab {tab} ({container.name}): {len(items)} items, {container.free_cells()} free cells")
            for cat in sorted(cats.keys()):
                print(f"  {cat}: {cats[cat]}")
            print()

    def plan_reorganization(self):
        """Plan item movements based on layout configuration."""
        moves = []  # list of (item, from_tab, to_tab)

        # Skip tab 0 (personal)
        moveable_items = [i for i in self.all_items if i["_tab"] != 0]

        for item in moveable_items:
            cat = item["_category"]
            current_tab = item["_tab"]

            # Find the target tab for this category
            target_tab = None
            for tab, cfg in self.layout.items():
                if tab == 0:
                    continue
                if cat in cfg.get("categories", []):
                    target_tab = tab
                    break

            # If no specific tab, goes to overflow (tab 9)
            if target_tab is None:
                target_tab = 9

            if target_tab != current_tab:
                moves.append({
                    "item": item,
                    "from_tab": current_tab,
                    "to_tab": target_tab,
                    "name": item["name"],
                    "category": cat
                })

        return moves

    def show_plan(self, moves):
        """Display the reorganization plan."""
        print(f"\n=== REORGANIZATION PLAN ({len(moves)} moves) ===\n")

        by_dest = {}
        for m in moves:
            dest = m["to_tab"]
            if dest not in by_dest:
                by_dest[dest] = []
            by_dest[dest].append(m)

        for tab in sorted(by_dest.keys()):
            tab_moves = by_dest[tab]
            name = self.layout.get(tab, {}).get("name", f"Tab {tab}")
            print(f"→ Tab {tab} ({name}): {len(tab_moves)} items incoming")
            cats = {}
            for m in tab_moves:
                cats[m["category"]] = cats.get(m["category"], 0) + 1
            for cat in sorted(cats.keys()):
                print(f"    {cat}: {cats[cat]} from various tabs")
            print()

        # Check if any destination tab would overflow
        print("=== SPACE CHECK ===")
        for tab in sorted(by_dest.keys()):
            current_count = len([i for i in self.all_items if i["_tab"] == tab])
            incoming = len(by_dest[tab])
            outgoing = len([m for m in moves if m["from_tab"] == tab])
            final = current_count + incoming - outgoing
            capacity = 10 * 16  # 160 cells, but items take multiple cells
            print(f"  Tab {tab}: {current_count} now, +{incoming} in, -{outgoing} out = ~{final} items")

    def execute_move(self, item, from_tab, to_tab) -> bool:
        """Move a single item from one tab to another.

        Pattern from kolbot:
        1. Switch to source tab
        2. Pick item to cursor
        3. Check cursor has item
        4. Switch to destination tab
        5. Find free spot
        6. Place item
        7. Check cursor is empty
        8. Retry up to 3 times
        """
        uid = item["unit_id"]
        name = item["name"]

        for attempt in range(3):
            # Switch to source tab
            self.mcp.call("switch_stash_tab", {"tab": from_tab})
            time.sleep(1.5)

            # Pick to cursor
            self.mcp.call("item_to_cursor", {"item_id": uid})
            time.sleep(0.5)

            # Verify cursor
            cursor = self.mcp.call("get_cursor_item")
            if not cursor.get("has_item"):
                print(f"  WARN: Failed to pick up {name} (attempt {attempt+1})")
                time.sleep(1)
                continue

            # Switch to destination tab
            self.mcp.call("switch_stash_tab", {"tab": to_tab})
            time.sleep(1.5)

            # Find free spot using grid
            grid = self.mcp.call("get_stash_grid", {"container": "stash"})
            # Find first free cell (simple 1x1 placement for now)
            placed = False
            for y in range(grid.get("height", 16)):
                for x in range(grid.get("width", 10)):
                    if grid["grid"][y][x] == 0:
                        self.mcp.call("cursor_to_container", {
                            "item_id": uid, "x": x, "y": y, "container": "stash"
                        })
                        time.sleep(0.5)

                        # Check cursor is empty
                        cursor = self.mcp.call("get_cursor_item")
                        if not cursor.get("has_item"):
                            print(f"  OK: {name} → tab {to_tab} at ({x},{y})")
                            placed = True
                            break
                if placed:
                    break

            if placed:
                return True

            # If still on cursor, try to put it back
            print(f"  WARN: Couldn't place {name} in tab {to_tab}, returning to tab {from_tab}")
            self.mcp.call("switch_stash_tab", {"tab": from_tab})
            time.sleep(1.5)
            # Find a spot in the original tab
            self.mcp.call("cursor_to_container", {
                "item_id": uid, "x": item["grid_x"], "y": item["grid_y"], "container": "stash"
            })
            time.sleep(0.5)
            return False

        return False

    def organize(self, dry_run=False, max_moves=None):
        """Execute the full reorganization."""
        moves = self.plan_reorganization()
        if not moves:
            print("Nothing to reorganize!")
            return

        self.show_plan(moves)

        if dry_run:
            print("\n[DRY RUN] No items moved.")
            return

        print(f"\n=== EXECUTING {len(moves)} MOVES ===\n")
        success = 0
        failed = 0

        for i, m in enumerate(moves):
            if max_moves and i >= max_moves:
                print(f"\nStopped after {max_moves} moves (limit reached)")
                break

            print(f"[{i+1}/{len(moves)}] Moving {m['name']} from tab {m['from_tab']} to tab {m['to_tab']}...")
            if self.execute_move(m["item"], m["from_tab"], m["to_tab"]):
                success += 1
            else:
                failed += 1

        print(f"\n=== DONE: {success} moved, {failed} failed ===")


# ---- CLI ----

def main():
    parser = argparse.ArgumentParser(description="Stash Organizer")
    parser.add_argument("--scan", action="store_true", help="Scan all tabs")
    parser.add_argument("--plan", action="store_true", help="Show reorganization plan")
    parser.add_argument("--organize", action="store_true", help="Execute reorganization")
    parser.add_argument("--dry-run", action="store_true", help="Show plan without moving")
    parser.add_argument("--max-moves", type=int, help="Limit number of moves")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    organizer = StashOrganizer(args.url)

    if not organizer.mcp.alive():
        print("ERROR: MCP server not responding")
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
