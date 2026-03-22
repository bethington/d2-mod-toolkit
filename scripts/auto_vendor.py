"""Auto-Vendor — Evaluate inventory/stash items and sell junk to NPC.

Walks to nearest vendor NPC, opens trade, and sells items that the
evaluator marks as "vendor". Supports inventory cleanup and stash
tab vendoring.

Usage:
    python auto_vendor.py                     # Vendor inventory items
    python auto_vendor.py --stash-tab 1       # Vendor items from stash tab 1
    python auto_vendor.py --dry-run           # Show what would be sold
    python auto_vendor.py --all-stash         # Vendor across all stash tabs

Requires: game running, character in town, MCP on port 21337.
"""

import json
import sys
import time
import argparse

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)

from item_evaluator import ItemEvaluator


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
        text = r.json().get("result", {}).get("content", [{}])[0].get("text", "")
        return json.loads(text) if text.startswith("{") else {"_raw": text}


# Known vendor NPCs by act
VENDORS = {
    1: [  # Act 1
        {"name": "Charsi", "class_id": 154},
        {"name": "Akara", "class_id": 148},
    ],
    2: [  # Act 2
        {"name": "Fara", "class_id": 177},
        {"name": "Drognan", "class_id": 178},
    ],
    3: [  # Act 3
        {"name": "Ormus", "class_id": 255},
        {"name": "Hratli", "class_id": 253},
    ],
    4: [  # Act 4
        {"name": "Jamella", "class_id": 405},
        {"name": "Halbu", "class_id": 257},
    ],
    5: [  # Act 5
        {"name": "Larzuk", "class_id": 511},
        {"name": "Malah", "class_id": 513},
        {"name": "Anya", "class_id": 512},
    ],
}


class AutoVendor:
    def __init__(self, mcp_url="http://127.0.0.1:21337"):
        self.mcp = McpClient(mcp_url)
        self.evaluator = ItemEvaluator(mcp_url)

    def find_vendor(self):
        """Find nearest vendor NPC."""
        state = self.mcp.call("get_player_state")
        act = state.get("act", 1)

        units = self.mcp.call("get_nearby_units", {"max_distance": 100})
        nearby = units.get("units", [])

        # Match against known vendors for this act
        act_vendors = VENDORS.get(act, [])
        vendor_names = {v["name"].lower() for v in act_vendors}

        for u in nearby:
            if u.get("type") == "monster":
                name = u.get("name", "").lower()
                if any(vn in name for vn in vendor_names):
                    return {
                        "name": u["name"],
                        "unit_id": u["unit_id"],
                        "position": u["position"],
                        "distance": u["distance"],
                    }

        # Fallback: try any NPC with known vendor class_id
        vendor_class_ids = {v["class_id"] for v in act_vendors}
        for u in nearby:
            if u.get("type") == "monster" and u.get("class_id") in vendor_class_ids:
                return {
                    "name": u["name"],
                    "unit_id": u["unit_id"],
                    "position": u["position"],
                    "distance": u["distance"],
                }

        return None

    def walk_to_vendor(self, vendor):
        """Walk to the vendor NPC."""
        pos = vendor["position"]
        print(f"  Walking to {vendor['name']} at ({pos['x']},{pos['y']})...")
        self.mcp.call("walk_to", {"x": pos["x"], "y": pos["y"]})
        time.sleep(max(vendor["distance"] / 10, 2))

    def open_trade(self, vendor):
        """Interact with vendor to open trade panel."""
        print(f"  Opening trade with {vendor['name']}...")
        self.mcp.call("interact_object", {"unit_id": vendor["unit_id"], "unit_type": 1})
        time.sleep(3)

    def sell_items(self, items, vendor, dry_run=False):
        """Sell a list of items to the vendor."""
        if not items:
            print("  No items to sell")
            return 0

        if dry_run:
            print(f"\n  [DRY RUN] Would sell {len(items)} items:")
            for item in items:
                print(f"    {item['unit_id']:5d} {item['name'][:30]:30s} ({item['reason']})")
            return 0

        sold = 0
        for item in items:
            uid = item["unit_id"]
            name = item["name"][:30]
            result = self.mcp.call("sell_item", {"item_id": uid, "npc_id": vendor["unit_id"]})
            time.sleep(0.3)

            # Verify sold
            check = self.mcp.call("get_item_stats", {"item_id": uid})
            if "Item not found" in check.get("_raw", "") or check.get("_raw") == "Item not found":
                print(f"    SOLD {name}")
                sold += 1
            else:
                print(f"    FAIL {name} (still exists)")

        return sold

    def vendor_inventory(self, dry_run=False):
        """Evaluate and sell vendor-worthy items from inventory."""
        print("\n=== AUTO-VENDOR: Inventory ===\n")

        # Get inventory items
        inv = self.mcp.call("get_inventory")
        inv_items = [i for i in inv.get("items", []) if i.get("storage") == "inventory"]

        # Evaluate each
        vendor_items = []
        for item in inv_items:
            verdict, reason, data = self.evaluator.evaluate_item(item["unit_id"])
            if verdict == "vendor":
                vendor_items.append({
                    "unit_id": item["unit_id"],
                    "name": data.get("name", item.get("name", "?")),
                    "quality": data.get("quality", "?"),
                    "reason": reason,
                })

        print(f"  {len(inv_items)} inventory items, {len(vendor_items)} to vendor")

        if not vendor_items:
            print("  Nothing to vendor!")
            return

        if dry_run:
            self.sell_items(vendor_items, None, dry_run=True)
            return

        # Find and walk to vendor
        vendor = self.find_vendor()
        if not vendor:
            print("  ERROR: No vendor NPC found nearby")
            return

        self.walk_to_vendor(vendor)
        self.open_trade(vendor)

        sold = self.sell_items(vendor_items, vendor)
        print(f"\n  Sold {sold}/{len(vendor_items)} items")

    def vendor_stash_tab(self, tab, dry_run=False):
        """Evaluate and sell vendor-worthy items from a stash tab."""
        print(f"\n=== AUTO-VENDOR: Stash Tab {tab} ===\n")

        # Walk to stash and open it
        if not dry_run:
            objects = self.mcp.call("get_nearby_objects", {"max_distance": 100})
            stash = None
            for o in objects.get("objects", []):
                if "stash" in o.get("name", "").lower() or o.get("class_id") == 267:
                    stash = o
                    break
            if stash:
                self.mcp.call("walk_to", {"x": stash["position"]["x"], "y": stash["position"]["y"]})
                time.sleep(2)
            self.mcp.call("open_stash")
            time.sleep(2)
            self.mcp.call("switch_stash_tab", {"tab": tab})
            time.sleep(0.5)

        # Evaluate items on this tab
        results = self.evaluator.scan_tab(tab)
        vendor_items = results.get("vendor", [])

        print(f"  {sum(len(v) for v in results.values())} items on tab, {len(vendor_items)} to vendor")

        if not vendor_items:
            print("  Nothing to vendor!")
            return

        if dry_run:
            for item in vendor_items:
                print(f"    {item['unit_id']:5d} {item['name'][:30]:30s} ({item['reason']})")
            return

        # Move vendor items to inventory first, then sell
        # Pick each item to inventory
        moved = []
        for item in vendor_items:
            uid = item["unit_id"]
            # Pick to cursor
            self.mcp.call("item_to_cursor", {"item_id": uid})
            time.sleep(0.3)
            # Place in inventory (find free spot)
            grid = self.mcp.call("get_stash_grid", {"container": "inventory"})
            placed = False
            for y, row in enumerate(grid.get("grid", [])):
                for x, cell in enumerate(row):
                    if cell == 0:
                        self.mcp.call("cursor_to_container", {
                            "item_id": uid, "x": x, "y": y, "container": "inventory"
                        })
                        time.sleep(0.3)
                        check = self.mcp.call("get_cursor_item")
                        if not check.get("has_item"):
                            moved.append(item)
                            placed = True
                            break
                if placed:
                    break
            if not placed:
                # Clear cursor back to stash
                self.mcp.call("cursor_to_container", {
                    "item_id": uid, "x": 0, "y": 0, "container": "stash"
                })
                time.sleep(0.3)
                print(f"  Can't fit {item['name'][:20]} in inventory, stopping batch")
                break

        if not moved:
            print("  No items moved to inventory")
            return

        # Close stash, walk to vendor, sell
        self.mcp.call("close_panels")
        time.sleep(1)

        vendor = self.find_vendor()
        if not vendor:
            print("  ERROR: No vendor NPC found")
            return

        self.walk_to_vendor(vendor)
        self.open_trade(vendor)

        sold = self.sell_items(moved, vendor)
        print(f"\n  Sold {sold}/{len(moved)} items from stash tab {tab}")


def main():
    parser = argparse.ArgumentParser(description="Auto-Vendor")
    parser.add_argument("--stash-tab", type=int, help="Vendor items from specific stash tab")
    parser.add_argument("--all-stash", action="store_true", help="Vendor across all stash tabs")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be sold")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    av = AutoVendor(args.url)

    # Check game state
    state = av.mcp.call("get_game_state")
    if state.get("state") != "in_game":
        print(f"ERROR: Not in game (state: {state.get('state', 'unknown')})")
        sys.exit(1)

    if args.stash_tab is not None:
        av.vendor_stash_tab(args.stash_tab, dry_run=args.dry_run)
    elif args.all_stash:
        for tab in range(1, 10):
            av.vendor_stash_tab(tab, dry_run=args.dry_run)
    else:
        av.vendor_inventory(dry_run=args.dry_run)


if __name__ == "__main__":
    main()
