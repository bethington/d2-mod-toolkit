"""Farming Loop — Automated farming for PD2 Sorceress.

Teleports to farming area, kills monsters, picks up loot,
returns to town to vendor/stash, and repeats.

Usage:
    python farming_loop.py                        # Default: farm Countess (area 25)
    python farming_loop.py --area 2               # Farm Cold Plains
    python farming_loop.py --area 2 --runs 5      # 5 runs
    python farming_loop.py --dry-run              # Show plan without acting

Requires: game running in town, MCP on port 21337.
"""

import json
import sys
import time
import argparse
import math

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


class McpClient:
    def __init__(self, url="http://127.0.0.1:21337"):
        self.url = url
        self._id = 0

    def call(self, tool, args=None, timeout=20):
        self._id += 1
        r = requests.post(f"{self.url}/mcp", json={
            "jsonrpc": "2.0", "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}
        }, timeout=timeout)
        text = r.json().get("result", {}).get("content", [{}])[0].get("text", "")
        return json.loads(text) if text.startswith("{") else {"_raw": text}


# Area IDs for waypoint destinations (Hell difficulty)
WAYPOINT_AREAS = {
    # Act 1
    1: "Rogue Encampment", 2: "Cold Plains", 3: "Stony Field",
    4: "Dark Wood", 5: "Black Marsh", 28: "Outer Cloister",
    29: "Jail Level 1", 32: "Inner Cloister", 35: "Catacombs Level 2",
    # Act 2
    40: "Lut Gholein", 42: "Sewers Level 2", 43: "Dry Hills",
    44: "Halls of the Dead Level 2", 45: "Far Oasis", 46: "Lost City",
    47: "Palace Cellar Level 1", 54: "Arcane Sanctuary", 74: "Canyon of the Magi",
    # Act 3
    75: "Kurast Docks", 76: "Spider Forest", 77: "Great Marsh",
    78: "Flayer Jungle", 79: "Lower Kurast", 80: "Kurast Bazaar",
    81: "Upper Kurast", 83: "Travincal", 100: "Durance of Hate Level 2",
    # Act 4
    103: "Pandemonium Fortress", 105: "City of the Damned", 107: "River of Flame",
    # Act 5
    109: "Harrogath", 111: "Frigid Highlands", 112: "Arreat Plateau",
    113: "Crystalline Passage", 114: "Halls of Pain", 115: "Glacial Trail",
    116: "Frozen Tundra", 118: "The Ancients' Way", 129: "Worldstone Keep Level 2",
}

# Town area IDs (for return waypoint)
TOWN_AREAS = {1: 1, 2: 40, 3: 75, 4: 103, 5: 109}


class FarmingLoop:
    def __init__(self, mcp_url="http://127.0.0.1:21337"):
        self.mcp = McpClient(mcp_url)
        self.stats = {"runs": 0, "kills": 0, "items_found": 0, "items_vendored": 0,
                      "deaths": 0, "start_time": time.time()}

    def get_state(self):
        """Get current game state."""
        return self.mcp.call("get_game_state")

    def get_position(self):
        """Get player position."""
        ps = self.mcp.call("get_player_state")
        return ps.get("position", {}).get("x", 0), ps.get("position", {}).get("y", 0)

    def get_hp_pct(self):
        """Get HP percentage."""
        ps = self.mcp.call("get_player_state")
        hp, max_hp = ps.get("hp", 0), ps.get("max_hp", 1)
        return hp / max_hp * 100 if max_hp > 0 else 0

    def find_waypoint(self):
        """Find nearest waypoint."""
        objects = self.mcp.call("get_nearby_objects", {"max_distance": 100})
        for o in objects.get("objects", []):
            if "waypoint" in o.get("name", "").lower() or o.get("class_id") in (119, 156, 157, 237, 238, 288, 323, 324, 398, 402, 429, 494, 496, 511, 539):
                return o
        return None

    def teleport_to(self, x, y):
        """Teleport (right-click skill) to a location."""
        self.mcp.call("cast_skill", {"x": x, "y": y})
        time.sleep(0.3)

    def ensure_mobile(self):
        """Check if character can move; if stuck, try to unstick."""
        px, py = self.get_position()
        self.mcp.call("walk_to", {"x": px + 5, "y": py + 5})
        time.sleep(1)
        px2, py2 = self.get_position()
        if px == px2 and py == py2:
            # Stuck! Try recovery steps
            print("  Character stuck, attempting recovery...")
            self.mcp.call("close_panels")
            time.sleep(0.5)
            self.mcp.call("walk_to", {"x": px + 5, "y": py + 5})
            time.sleep(2)
            px3, py3 = self.get_position()
            if px == px3 and py == py3:
                print("  Still stuck after close_panels")
                return False
        return True

    def travel_to_area(self, area_id):
        """Use waypoint to travel to an area."""
        # Close any open panels first
        self.mcp.call("close_panels")
        time.sleep(0.5)

        wp = self.find_waypoint()
        if not wp:
            print("  ERROR: No waypoint found nearby")
            return False

        print(f"  Waypoint to area {area_id} ({WAYPOINT_AREAS.get(area_id, '?')})...")

        # Walk close to waypoint
        self.mcp.call("walk_to", {"x": wp["position"]["x"], "y": wp["position"]["y"]})
        time.sleep(5)

        # Send use_waypoint (sends interact + destination packet internally)
        self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": area_id},
                      timeout=10)

        # Wait for area change (up to 15 seconds for loading)
        for i in range(15):
            time.sleep(1)
            gs = self.get_state()
            if gs.get("area") == area_id:
                print(f"  Arrived at {gs.get('area_name', '?')}")
                return True
            if gs.get("state") != "in_game":
                time.sleep(5)  # loading screen
                continue

        # Retry once
        print("  First attempt failed, retrying...")
        self.mcp.call("walk_to", {"x": wp["position"]["x"], "y": wp["position"]["y"]})
        time.sleep(4)
        self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": area_id},
                      timeout=10)
        time.sleep(10)

        gs = self.get_state()
        if gs.get("area") == area_id:
            print(f"  Arrived at {gs.get('area_name', '?')}")
            return True

        print(f"  Failed to travel (area={gs.get('area', '?')}: {gs.get('area_name', '?')})")
        return False

    def return_to_town(self):
        """Return to town via waypoint or TP."""
        gs = self.get_state()
        area = gs.get("area", 0)
        area_name = gs.get("area_name", "?")

        # Determine which act we're in and the town area
        act = gs.get("act", 1) if "act" in gs else 1
        # Rough act detection from area ID
        if area <= 39: act = 1
        elif area <= 74: act = 2
        elif area <= 102: act = 3
        elif area <= 108: act = 4
        else: act = 5

        town_area = TOWN_AREAS.get(act, 1)

        # Check if already in town
        if area == town_area:
            print(f"  Already in town ({area_name})")
            return True

        # Find waypoint to go back
        wp = self.find_waypoint()
        if wp:
            print(f"  Returning to town via waypoint...")
            self.mcp.call("walk_to", {"x": wp["position"]["x"], "y": wp["position"]["y"]})
            time.sleep(3)
            self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": town_area})
            time.sleep(8)
            gs = self.get_state()
            if gs.get("area") == town_area:
                print(f"  Back in town ({gs.get('area_name', '?')})")
                return True

        print("  Could not return to town")
        return False

    def _is_attackable(self, unit):
        """Check if a unit is a valid attack target (not merc, NPC, or dead)."""
        if unit.get("type") != "monster":
            return False
        if unit.get("dead", False):
            return False

        name = unit.get("name", "")
        hp = unit.get("hp", 0) >> 8
        max_hp = unit.get("max_hp", 0) >> 8

        # Skip dead/no-HP units
        if max_hp <= 0:
            return False

        # Skip town NPCs (name = "an evil force" with 0 or low HP, or known NPC names)
        skip_names = {"an evil force", "charsi", "akara", "gheed", "warriv", "kashya",
                      "deckard cain", "fara", "drognan", "lysander", "greiz", "elzix",
                      "meshif", "jerhyn", "atma", "ormus", "hratli", "alkor", "asheara",
                      "natalya", "tyrael", "jamella", "halbu", "cain",
                      "larzuk", "qual-kehk", "malah", "anya", "nihlathak"}
        if name.lower() in skip_names:
            return False

        # Skip mercenaries (class_id 271=A1 Rogue, 338=A2 Guard, 359=A3 Iron Wolf, 560=A5 Barb)
        merc_classes = {271, 338, 359, 560, 561}
        if unit.get("class_id") in merc_classes:
            return False

        return True

    def scan_and_attack(self, max_duration=60):
        """Scan for monsters and attack them."""
        start = time.time()
        total_killed = 0
        teleport_count = 0

        while time.time() - start < max_duration:
            # Get nearby monsters
            units = self.mcp.call("get_nearby_units", {"max_distance": 40})
            monsters = [u for u in units.get("units", []) if self._is_attackable(u)]

            if not monsters:
                # No monsters nearby, teleport to explore
                px, py = self.get_position()
                import random
                angle = random.uniform(0, 2 * math.pi)
                dist = random.uniform(15, 30)
                tx = int(px + dist * math.cos(angle))
                ty = int(py + dist * math.sin(angle))
                self.teleport_to(tx, ty)
                teleport_count += 1
                time.sleep(0.4)

                # After many teleports with no monsters, area might be clear
                if teleport_count > 20:
                    print(f"    Explored {teleport_count} teleports, area seems clear")
                    break
                continue

            # Attack monsters in range
            for target in monsters[:3]:  # attack up to 3 nearby
                immunities = target.get("immunities", [])
                immune_str = f" IMMUNE:{','.join(immunities)}" if immunities else ""
                hp = target.get("hp", 0) >> 8

                # Skip lightning immunes (we're a Lightning Sorc)
                if "lightning" in immunities:
                    continue

                print(f"    >> {target['name']} HP:{hp}{immune_str}")

                # Cast right-click skill on the monster (Chain Lightning)
                for cast in range(3):
                    self.mcp.call("cast_skill", {"unit_id": target["unit_id"], "unit_type": 1})
                    time.sleep(0.3)

                total_killed += 1  # approximate

            # Safety: chicken if HP low
            hp_pct = self.get_hp_pct()
            if hp_pct < 20:
                print(f"    CHICKEN! HP at {hp_pct:.0f}% — exiting game!")
                self.mcp.call("exit_game")
                self.stats["deaths"] += 1
                time.sleep(5)
                return total_killed
            elif hp_pct < 50:
                print(f"    LOW HP ({hp_pct:.0f}%) — retreating to safe distance")
                px, py = self.get_position()
                self.teleport_to(px - 20, py - 20)
                time.sleep(1)

            teleport_count = 0  # reset after finding monsters

        return total_killed

    def pick_up_loot(self):
        """Pick up items on the ground."""
        units = self.mcp.call("get_nearby_units", {"max_distance": 20})
        items = [u for u in units.get("units", []) if u.get("type") == "item"]

        picked = 0
        for item in items[:5]:  # limit to 5 items per sweep
            self.mcp.call("pickup_item", {"item_id": item["unit_id"]})
            time.sleep(0.3)
            picked += 1

        return picked

    def vendor_and_stash(self):
        """Vendor junk and stash good items."""
        from item_evaluator import ItemEvaluator
        evaluator = ItemEvaluator(self.mcp.url)

        # Get inventory items
        inv = self.mcp.call("get_inventory")
        inv_items = [i for i in inv.get("items", []) if i.get("storage") == "inventory"]

        vendor_items = []
        for item in inv_items:
            verdict, reason, data = evaluator.evaluate_item(item["unit_id"])
            if verdict == "vendor":
                vendor_items.append({"unit_id": item["unit_id"], "name": data.get("name", "?")})

        if not vendor_items:
            return 0

        # Find vendor NPC
        units = self.mcp.call("get_nearby_units", {"max_distance": 100})
        vendor = None
        for u in units.get("units", []):
            if u.get("type") == "monster" and u.get("name", "").lower() not in ("an evil force", ""):
                # Try known vendor names
                name = u.get("name", "").lower()
                if any(v in name for v in ["charsi", "akara", "gheed", "fara", "drognan",
                                            "ormus", "hratli", "jamella", "halbu",
                                            "larzuk", "malah", "anya", "qual-kehk"]):
                    vendor = u
                    break

        if not vendor:
            return 0

        # Walk to vendor and sell
        self.mcp.call("walk_to", {"x": vendor["position"]["x"], "y": vendor["position"]["y"]})
        time.sleep(2)
        self.mcp.call("interact_object", {"unit_id": vendor["unit_id"], "unit_type": 1})
        time.sleep(3)

        sold = 0
        for item in vendor_items:
            self.mcp.call("sell_item", {"item_id": item["unit_id"], "npc_id": vendor["unit_id"]})
            time.sleep(0.3)
            sold += 1

        return sold

    def run_single(self, area_id, farm_duration=60):
        """Run a single farming loop."""
        self.stats["runs"] += 1
        run_start = time.time()
        print(f"\n=== Run {self.stats['runs']} ===")

        # Pre-check: ensure character can move
        if not self.ensure_mobile():
            print("  Character is stuck, skipping run")
            return False

        # Close any open panels
        self.mcp.call("close_panels")
        time.sleep(0.5)

        # Step 1: Travel to farming area
        if not self.travel_to_area(area_id):
            return False

        # Step 2: Farm (teleport + kill + loot)
        print(f"  Farming for {farm_duration}s...")
        killed = self.scan_and_attack(max_duration=farm_duration)
        self.stats["kills"] += killed
        print(f"  Killed {killed} monsters")

        # Step 3: Pick up loot
        picked = self.pick_up_loot()
        self.stats["items_found"] += picked
        if picked > 0:
            print(f"  Picked up {picked} items")

        # Step 4: Return to town
        if not self.return_to_town():
            return False

        # Step 5: Vendor junk
        vendored = self.vendor_and_stash()
        self.stats["items_vendored"] += vendored
        if vendored > 0:
            print(f"  Vendored {vendored} items")

        elapsed = time.time() - run_start
        print(f"  Run complete in {elapsed:.0f}s")
        return True

    def print_stats(self):
        """Print farming statistics."""
        elapsed = time.time() - self.stats["start_time"]
        mins = elapsed / 60
        print(f"\n=== Farming Stats ===")
        print(f"  Runs:      {self.stats['runs']}")
        print(f"  Kills:     {self.stats['kills']}")
        print(f"  Items:     {self.stats['items_found']} found, {self.stats['items_vendored']} vendored")
        print(f"  Deaths:    {self.stats['deaths']}")
        print(f"  Time:      {mins:.1f} minutes")
        if self.stats["runs"] > 0:
            print(f"  Avg run:   {elapsed/self.stats['runs']:.0f}s")
        if mins > 0:
            print(f"  Kills/min: {self.stats['kills']/mins:.1f}")


def main():
    parser = argparse.ArgumentParser(description="Farming Loop")
    parser.add_argument("--area", type=int, default=2, help="Area ID to farm (default: 2=Cold Plains)")
    parser.add_argument("--runs", type=int, default=3, help="Number of runs (default: 3)")
    parser.add_argument("--duration", type=int, default=60, help="Farm duration per run in seconds (default: 60)")
    parser.add_argument("--dry-run", action="store_true", help="Show plan only")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    farm = FarmingLoop(args.url)

    # Check game state
    gs = farm.get_state()
    if gs.get("state") != "in_game":
        print(f"ERROR: Not in game (state: {gs.get('state', '?')})")
        sys.exit(1)

    area_name = WAYPOINT_AREAS.get(args.area, f"Area {args.area}")
    print(f"Farming: {area_name} (area {args.area})")
    print(f"Runs: {args.runs}, Duration: {args.duration}s per run")

    if args.dry_run:
        print("\n[DRY RUN] Would farm:")
        print(f"  1. Waypoint to {area_name}")
        print(f"  2. Teleport + kill for {args.duration}s")
        print(f"  3. Pick up loot")
        print(f"  4. Return to town")
        print(f"  5. Vendor junk")
        print(f"  6. Repeat {args.runs} times")
        return

    for run in range(args.runs):
        success = farm.run_single(args.area, farm_duration=args.duration)
        if not success:
            print("Run failed, stopping")
            break

    farm.print_stats()


if __name__ == "__main__":
    main()
