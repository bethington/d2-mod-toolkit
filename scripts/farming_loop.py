"""Farming Loop — Automated farming for PD2 Sorceress.

Teleports to farming area, kills monsters, picks up loot,
returns to town to vendor/stash, creates new game, and repeats.

Usage:
    python farming_loop.py --area 129 --runs 5             # 5 runs in WSK L2
    python farming_loop.py --area 129 --runs 10 --new-game # New game each run
    python farming_loop.py --area 129 --duration 60        # 60s per run
    python farming_loop.py --dry-run --area 129             # Show plan only
    python farming_loop.py --rotate --runs 10 --new-game   # Rotate areas

Requires: game running in town (or use --new-game to auto-start).
"""

import json
import sys
import os
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
        try:
            r = requests.post(f"{self.url}/mcp", json={
                "jsonrpc": "2.0", "id": self._id,
                "method": "tools/call",
                "params": {"name": tool, "arguments": args or {}}
            }, timeout=timeout)
            text = r.json().get("result", {}).get("content", [{}])[0].get("text", "")
            return json.loads(text) if text.startswith("{") else {"_raw": text}
        except (requests.ConnectionError, requests.Timeout):
            return {"_error": "MCP not responding", "_raw": ""}


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


# Farming area rotation — good Hell areas for Lightning Sorc
FARM_ROTATION = [
    129,  # Worldstone Keep Level 2
    107,  # River of Flame
    83,   # Travincal
    100,  # Durance of Hate Level 2
    54,   # Arcane Sanctuary
    35,   # Catacombs Level 2
]


class FarmingLoop:
    def __init__(self, mcp_url="http://127.0.0.1:21337"):
        self.mcp = McpClient(mcp_url)
        self.stats = {"runs": 0, "kills": 0, "items_found": 0, "items_vendored": 0,
                      "items_picked": 0, "items_verified": 0,
                      "deaths": 0, "start_time": time.time()}
        self._pathfinder = None

    # Known-good skill mappings (verified by testing damage on monsters)
    KNOWN_SKILLS = {
        "combustion": {"teleport": 394, "combat": 54},  # Lightning Sorc
    }

    def detect_skills(self):
        """Detect Teleport and combat skills. Uses cached mappings for known characters."""
        ps = self.mcp.call("get_player_state")
        char_name = ps.get("name", "").lower()

        # Use known-good mapping if available
        if char_name in self.KNOWN_SKILLS:
            mapping = self.KNOWN_SKILLS[char_name]
            self.SKILL_TELEPORT = mapping["teleport"]
            self.SKILL_COMBAT = mapping["combat"]
            print(f"  Skills (cached): Teleport={self.SKILL_TELEPORT}, Combat={self.SKILL_COMBAT}")
            return True

        # Fallback: auto-detect
        skills = self.mcp.call("get_skills")
        all_skills = skills.get("skills", [])

        # Find Teleport: PD2 ID 394
        for sid in [394]:
            if any(s["skill_id"] == sid for s in all_skills):
                self.SKILL_TELEPORT = sid
                break

        if not self.SKILL_TELEPORT:
            one_pointers = [s for s in all_skills if s["base_level"] <= 2]
            for s in one_pointers:
                self.mcp.call("switch_skill", {"skill_id": s["skill_id"]})
                time.sleep(0.2)
                pos1 = self.get_position()
                self.mcp.call("cast_skill", {"x": pos1[0] + 15, "y": pos1[1] + 10})
                time.sleep(0.5)
                pos2 = self.get_position()
                if abs(pos2[0] - pos1[0]) + abs(pos2[1] - pos1[1]) > 5:
                    self.SKILL_TELEPORT = s["skill_id"]
                    break

        # Find combat skill: highest total_level skill (excluding teleport)
        candidates = [s for s in all_skills if s["base_level"] >= 10
                      and s["skill_id"] != self.SKILL_TELEPORT]
        candidates.sort(key=lambda s: s["total_level"], reverse=True)

        # Test on nearby monster if available
        units = self.mcp.call("get_nearby_units", {"max_distance": 100})
        test_monsters = [u for u in units.get("units", []) if u.get("type") == "monster"
                        and not u.get("dead") and u.get("hp", 0) > 0
                        and u.get("class_id", 0) not in {271, 338, 359, 560, 561}
                        and u.get("name", "") not in ("an evil force",)]

        if test_monsters and candidates:
            for cand in candidates[:5]:
                sid = cand["skill_id"]
                units_fresh = self.mcp.call("get_nearby_units", {"max_distance": 100})
                alive = [u for u in units_fresh.get("units", []) if u.get("type") == "monster"
                        and not u.get("dead") and u.get("hp", 0) > 0
                        and u.get("class_id", 0) not in {271, 338, 359, 560, 561}
                        and u.get("name", "") not in ("an evil force",)]
                if not alive:
                    break
                target = min(alive, key=lambda m: m["distance"])

                if self.SKILL_TELEPORT:
                    self.mcp.call("switch_skill", {"skill_id": self.SKILL_TELEPORT})
                    time.sleep(0.1)
                    self.mcp.call("cast_skill", {"x": target["position"]["x"], "y": target["position"]["y"]})
                    time.sleep(0.3)

                self.mcp.call("switch_skill", {"skill_id": sid})
                time.sleep(0.2)
                hp_before = target.get("hp", 0)
                for _ in range(8):
                    self.mcp.call("cast_skill", {"x": target["position"]["x"], "y": target["position"]["y"]})
                    time.sleep(0.15)

                units2 = self.mcp.call("get_nearby_units", {"max_distance": 80})
                m2 = [u for u in units2.get("units", []) if u.get("unit_id") == target["unit_id"]]
                if not m2 or m2[0].get("hp", 0) < hp_before:
                    self.SKILL_COMBAT = sid
                    print(f"    (verified skill {sid} deals damage)")
                    break

        if not self.SKILL_COMBAT and candidates:
            self.SKILL_COMBAT = candidates[0]["skill_id"]

        print(f"  Skills: Teleport={self.SKILL_TELEPORT}, Combat={self.SKILL_COMBAT}")
        return self.SKILL_TELEPORT is not None and self.SKILL_COMBAT is not None

    def create_new_game(self, character="combustion"):
        """Exit current game and create a new one (resets monsters).

        Handles MCP crash on quit_game by waiting for game process to die,
        then relaunching via game_manager.
        """
        import subprocess
        print("  Creating new game...")

        # Try graceful quit (MCP may crash during menu transition)
        try:
            self.mcp.call("quit_game", timeout=5)
        except Exception:
            pass  # Expected — MCP often crashes on game exit

        # Wait for game to fully exit or MCP to go down
        for i in range(15):
            time.sleep(2)
            try:
                self.mcp.call("ping", timeout=3)
                # Still alive — game hasn't exited yet, that's ok
            except Exception:
                break  # MCP down, game likely exiting

        # Extra wait for process cleanup
        time.sleep(3)

        # Relaunch via game_manager (handles kill, launch, navigate)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        for attempt in range(2):
            result = subprocess.run(
                ["python", "game_manager.py", "--character", character, "--no-deploy"],
                capture_output=True, text=True, timeout=180,
                cwd=script_dir
            )

            if "Ready!" in result.stdout or "in game" in result.stdout.lower():
                print("  New game created")
                time.sleep(2)

                # Reset pathfinder cache (new level layout)
                self._pathfinder = None

                # Verify in-game state
                try:
                    gs = self.get_state()
                    if gs.get("state") == "in_game":
                        diff = gs.get("difficulty", 0)
                        if diff < 2:
                            print(f"  WARNING: Loaded in difficulty {diff}, wanted Hell (2)")
                        return True
                except Exception:
                    pass

            if attempt == 0:
                print("  First attempt failed, retrying...")
                time.sleep(5)

        print("  Failed to create new game after 2 attempts")
        return False

    def get_state(self):
        """Get current game state."""
        return self.mcp.call("get_game_state")

    def get_position(self):
        """Get player position."""
        ps = self.mcp.call("get_player_state")
        return ps.get("position", {}).get("x", 0), ps.get("position", {}).get("y", 0)

    def get_hp_pct(self):
        """Get HP percentage. Returns 100 if data unavailable (don't chicken on bad data)."""
        ps = self.mcp.call("get_player_state")
        if ps.get("_error") or "hp" not in ps:
            return 100  # assume safe if can't read
        hp, max_hp = ps.get("hp", 0), ps.get("max_hp", 1)
        return hp / max_hp * 100 if max_hp > 0 else 100

    def find_waypoint(self):
        """Find nearest waypoint."""
        objects = self.mcp.call("get_nearby_objects", {"max_distance": 100})
        for o in objects.get("objects", []):
            if "waypoint" in o.get("name", "").lower() or o.get("class_id") in (119, 156, 157, 237, 238, 288, 323, 324, 398, 402, 429, 494, 496, 511, 539):
                return o
        return None

    # PD2 Skill IDs (auto-detected or configured)
    SKILL_TELEPORT = None
    SKILL_COMBAT = None

    def teleport_to(self, x, y):
        """Teleport to a location using Teleport skill."""
        self.mcp.call("switch_skill", {"skill_id": self.SKILL_TELEPORT})
        time.sleep(0.1)
        self.mcp.call("cast_skill", {"x": x, "y": y})
        time.sleep(0.3)

    def switch_to_combat(self):
        """Switch right-click to combat skill."""
        self.mcp.call("switch_skill", {"skill_id": self.SKILL_COMBAT})
        time.sleep(0.1)

    def ensure_mobile(self):
        """Check if character can move; if stuck, try multiple directions."""
        px, py = self.get_position()
        for dx, dy in [(5,5), (-5,-5), (5,0), (0,5), (-5,0), (0,-5), (5,-5), (-5,5)]:
            self.mcp.call("walk_to", {"x": px + dx, "y": py + dy, "run": True})
            time.sleep(0.8)
            px2, py2 = self.get_position()
            if px != px2 or py != py2:
                return True
        # All directions failed — try closing panels
        print("  Character stuck, attempting recovery...")
        self.mcp.call("close_panels")
        time.sleep(0.5)
        self.mcp.call("walk_to", {"x": px + 5, "y": py + 5, "run": True})
        time.sleep(1)
        px3, py3 = self.get_position()
        if px != px3 or py != py3:
            return True
        print("  Still stuck after close_panels")
        return False

    def travel_to_area(self, area_id):
        """Use waypoint to travel to an area."""
        self.mcp.call("close_panels")
        time.sleep(0.5)

        wp = self.find_waypoint()
        if not wp:
            # Try using level exits to find waypoint location
            exits = self.mcp.call("get_level_exits")
            wp_exits = [e for e in exits.get("exits", []) if e.get("type") == "waypoint"]
            if wp_exits:
                # Teleport toward waypoint
                print(f"  Teleporting toward waypoint at ({wp_exits[0]['x']},{wp_exits[0]['y']})...")
                if self.SKILL_TELEPORT:
                    self.teleport_toward(wp_exits[0]["x"], wp_exits[0]["y"], max_hops=15)
                    time.sleep(1)
                    wp = self.find_waypoint()

            if not wp:
                print("  ERROR: No waypoint found nearby")
                return False

        print(f"  Waypoint to area {area_id} ({WAYPOINT_AREAS.get(area_id, '?')})...")

        # Walk step-by-step to waypoint (5 units at a time)
        wp_x, wp_y = wp["position"]["x"], wp["position"]["y"]
        for _ in range(12):
            ps = self.mcp.call("get_player_state")
            cx, cy = ps["position"]["x"], ps["position"]["y"]
            dx, dy = wp_x - cx, wp_y - cy
            if abs(dx) + abs(dy) < 6:
                break
            sx = max(-5, min(5, dx))
            sy = max(-5, min(5, dy))
            self.mcp.call("walk_to", {"x": cx + sx, "y": cy + sy, "run": True})
            time.sleep(0.8)

        # Open waypoint — try interact_entity first, fall back to interact_object
        result = self.mcp.call("interact_entity", {
            "unit_id": wp["unit_id"], "unit_type": 2,
            "expected_panel": "waypoint", "timeout_ms": 8000
        })
        if result.get("status") != "opened":
            # Fallback: raw interact packet + wait
            print(f"  interact_entity failed ({result.get('status', '?')}), trying raw interact...")
            self.mcp.call("interact_object", {"unit_id": wp["unit_id"], "unit_type": 2})
            time.sleep(3)

        # Travel
        self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": area_id},
                      timeout=10)
        time.sleep(3)

        # Close waypoint panel (critical!)
        self.mcp.call("close_panels")
        time.sleep(1)

        gs = self.get_state()
        if gs.get("area") == area_id:
            print(f"  Arrived at {gs.get('area_name', '?')}")
            return True

        print(f"  Failed to travel (area={gs.get('area', '?')}: {gs.get('area_name', '?')})")
        return False

    def _get_pathfinder(self):
        """Lazy-init pathfinder."""
        if self._pathfinder is None:
            try:
                from teleport_path import TeleportPathfinder
                self._pathfinder = TeleportPathfinder(self.mcp)
            except ImportError:
                pass
        return self._pathfinder

    def teleport_toward(self, target_x, target_y, max_hops=20, hop_range=30, chicken=False):
        """Teleport toward a target position using A* pathfinding if available,
        falling back to straight-line hops.

        Args:
            chicken: If True, exit game when HP drops critically low (only during combat)
        Returns True if we got within hop_range of the target.
        """
        if not self.SKILL_TELEPORT:
            return False

        # Try A* pathfinding first
        pf = self._get_pathfinder()
        if pf:
            path = pf.find_path(target_x, target_y, max_hops=max_hops)
            if path:
                print(f"    A* path: {len(path)} hops")
                for wx, wy in path:
                    self.teleport_to(wx, wy)
                    time.sleep(0.3)
                return True

        # Fallback: straight-line with stuck detection
        for hop in range(max_hops):
            px, py = self.get_position()
            dx, dy = target_x - px, target_y - py
            dist = math.sqrt(dx * dx + dy * dy)

            if dist <= hop_range:
                return True

            ratio = min(hop_range / dist, 1.0)
            tx = int(px + dx * ratio)
            ty = int(py + dy * ratio)
            self.teleport_to(tx, ty)
            time.sleep(0.3)

            # Stuck detection
            px2, py2 = self.get_position()
            if abs(px2 - px) < 2 and abs(py2 - py) < 2:
                offset_x = int(px + dy * 0.3)
                offset_y = int(py - dx * 0.3)
                self.teleport_to(offset_x, offset_y)
                time.sleep(0.3)

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

        # Find waypoint nearby first
        wp = self.find_waypoint()
        if wp:
            return self._use_waypoint_to_town(wp, town_area)

        # No waypoint nearby — use get_level_exits to find waypoint position
        print("  No waypoint nearby, searching via level exits...")
        exits = self.mcp.call("get_level_exits")
        wp_exits = [e for e in exits.get("exits", []) if e.get("type") == "waypoint"]

        if wp_exits and self.SKILL_TELEPORT:
            wp_pos = wp_exits[0]
            print(f"  Teleporting toward waypoint at ({wp_pos['x']},{wp_pos['y']})...")
            reached = self.teleport_toward(wp_pos["x"], wp_pos["y"], max_hops=30)

            if reached:
                # Now we should be close enough to find it
                time.sleep(0.5)
                wp = self.find_waypoint()
                if wp:
                    return self._use_waypoint_to_town(wp, town_area)

            # Still can't find it — try walking the last bit
            self.mcp.call("walk_to", {"x": wp_pos["x"], "y": wp_pos["y"]})
            time.sleep(4)
            wp = self.find_waypoint()
            if wp:
                return self._use_waypoint_to_town(wp, town_area)

        # No waypoint exits found — try level exit tiles to get to adjacent level
        level_exits = [e for e in exits.get("exits", []) if e.get("type") == "exit"]
        if level_exits and self.SKILL_TELEPORT:
            # Teleport to nearest level exit — might lead closer to a waypoint
            px, py = self.get_position()
            nearest = min(level_exits, key=lambda e: math.sqrt(
                (e["x"] - px) ** 2 + (e["y"] - py) ** 2))
            print(f"  Teleporting toward level exit at ({nearest['x']},{nearest['y']})...")
            self.teleport_toward(nearest["x"], nearest["y"], max_hops=30)
            time.sleep(2)

            # Check if we changed areas
            gs2 = self.get_state()
            if gs2.get("area") != area:
                print(f"  Moved to {gs2.get('area_name', '?')}, retrying return_to_town...")
                return self.return_to_town()  # recursive retry from new area

        # Last resort: cast Town Portal
        if self._cast_town_portal(town_area):
            return True

        print("  Could not return to town")
        return False

    def _cast_town_portal(self, town_area):
        """Cast Town Portal and use it to return to town."""
        print("  Attempting Town Portal...")
        px, py = self.get_position()

        # Switch right skill to Town Portal (skill ID 220 in vanilla D2)
        self.mcp.call("switch_skill", {"skill_id": 220})
        time.sleep(0.3)

        # Cast at player's feet
        self.mcp.call("cast_skill", {"x": px, "y": py})
        time.sleep(1.5)

        # Look for the portal object nearby
        for attempt in range(5):
            objects = self.mcp.call("get_nearby_objects", {"max_distance": 20})
            for o in objects.get("objects", []):
                name = o.get("name", "").lower()
                if "portal" in name or o.get("class_id") == 59:  # 59 = Town Portal
                    print(f"  Found portal, interacting...")
                    self.mcp.call("interact_object", {
                        "unit_id": o["unit_id"], "unit_type": 2})
                    time.sleep(5)

                    gs = self.get_state()
                    if gs.get("area") == town_area:
                        print(f"  Back in town via TP ({gs.get('area_name', '?')})")
                        return True
            time.sleep(1)

        # Switch back to teleport
        if self.SKILL_TELEPORT:
            self.mcp.call("switch_skill", {"skill_id": self.SKILL_TELEPORT})

        return False

    def _use_waypoint_to_town(self, wp, town_area):
        """Walk to waypoint and use it to return to town."""
        print(f"  Returning to town via waypoint...")
        self.mcp.call("walk_to", {"x": wp["position"]["x"], "y": wp["position"]["y"]})
        time.sleep(3)
        self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": town_area})
        time.sleep(8)
        gs = self.get_state()
        if gs.get("area") == town_area:
            print(f"  Back in town ({gs.get('area_name', '?')})")
            return True
        # Retry once
        print("  Waypoint didn't work, retrying...")
        self.mcp.call("walk_to", {"x": wp["position"]["x"], "y": wp["position"]["y"]})
        time.sleep(4)
        self.mcp.call("use_waypoint", {"waypoint_id": wp["unit_id"], "destination": town_area})
        time.sleep(8)
        gs = self.get_state()
        if gs.get("area") == town_area:
            print(f"  Back in town ({gs.get('area_name', '?')})")
            return True
        print("  Waypoint failed after retry")
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

        # Skip dead units (check raw HP, not shifted)
        raw_hp = unit.get("hp", 0)
        raw_max = unit.get("max_hp", 0)
        if raw_hp <= 0 and raw_max <= 0:
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

    def _build_explore_targets(self):
        """Build a list of walkable exploration targets from the collision map,
        sorted by distance from player (furthest first for systematic coverage)."""
        import random
        pf = self._get_pathfinder()
        if not pf or not pf.load_collision_map():
            return []

        px, py = self.get_position()
        gx, gy = pf.game_to_grid(px, py)

        # Sample walkable cells spread across the map
        targets = []
        step = max(pf._grid_w // 10, 3)
        for x in range(0, pf._grid_w, step):
            for y in range(0, pf._grid_h, step):
                if pf._is_walkable(x, y):
                    dist = math.sqrt((x - gx) ** 2 + (y - gy) ** 2)
                    game_x, game_y = pf.grid_to_game(x, y)
                    targets.append((game_x, game_y, dist))

        # Shuffle targets at similar distances for variety, then sort by distance
        random.shuffle(targets)
        targets.sort(key=lambda t: t[2])
        return [(t[0], t[1]) for t in targets]

    def scan_and_attack(self, max_duration=60):
        """Scan for monsters and attack them. Uses collision map for smart exploration."""
        import random
        start = time.time()
        total_killed = 0
        teleport_count = 0
        visited_positions = set()

        # Reveal map if tool is available (best-effort)
        try:
            self.mcp.call("reveal_map", timeout=5)
        except Exception:
            pass

        # Build exploration targets from collision map
        explore_targets = self._build_explore_targets()
        explore_idx = 0

        while time.time() - start < max_duration:
            # Get nearby monsters
            units = self.mcp.call("get_nearby_units", {"max_distance": 60})
            monsters = [u for u in units.get("units", []) if self._is_attackable(u)]

            if not monsters:
                # No monsters nearby — smart exploration
                px, py = self.get_position()
                visited_positions.add((px // 20, py // 20))  # coarse grid

                if explore_targets and explore_idx < len(explore_targets):
                    # Use collision-map-guided target
                    tx, ty = explore_targets[explore_idx]
                    explore_idx += 1
                    # Skip if already visited this area
                    if (tx // 20, ty // 20) in visited_positions:
                        continue
                    self.teleport_to(tx, ty)
                else:
                    # Fallback: random direction
                    angle = random.uniform(0, 2 * math.pi)
                    dist = random.uniform(15, 30)
                    tx = int(px + dist * math.cos(angle))
                    ty = int(py + dist * math.sin(angle))
                    self.teleport_to(tx, ty)

                teleport_count += 1
                time.sleep(0.3)

                if teleport_count > 60:
                    print(f"    Explored {teleport_count} teleports, area seems clear")
                    break
                continue

            # Attack monsters: teleport close then cast
            for target in monsters[:5]:
                immunities = target.get("immunities", [])
                immune_str = f" IMMUNE:{','.join(immunities)}" if immunities else ""
                hp = target.get("hp", 0)

                if "lightning" in immunities:
                    continue

                # Teleport on top of monster
                self.teleport_to(target["position"]["x"], target["position"]["y"])
                time.sleep(0.2)

                # Switch to combat and attack at monster position
                self.switch_to_combat()
                print(f"    >> {target['name']} HP:{hp}{immune_str}")
                for cast in range(6):
                    self.mcp.call("cast_skill", {"x": target["position"]["x"], "y": target["position"]["y"]})
                    time.sleep(0.15)
                    # Mid-combat HP check every 2 casts
                    if cast % 2 == 1:
                        hp_pct = self.get_hp_pct()
                        if hp_pct < 40:
                            print(f"    LOW HP ({hp_pct:.0f}%) mid-combat, retreating!")
                            px, py = self.get_position()
                            self.teleport_to(px - 25, py - 25)
                            time.sleep(0.5)
                            break

                total_killed += 1

            # Safety: heal at town if HP low, only chicken at lethal levels
            hp_pct = self.get_hp_pct()
            if hp_pct < 15:
                print(f"    CHICKEN! HP at {hp_pct:.0f}% — quitting game!")
                try:
                    self.mcp.call("quit_game", timeout=5)
                except Exception:
                    pass
                self.stats["deaths"] += 1
                time.sleep(8)
                return total_killed
            elif hp_pct < 40:
                print(f"    LOW HP ({hp_pct:.0f}%) — returning to town to heal")
                px, py = self.get_position()
                self.teleport_to(px + 30, py - 30)  # get away from monsters
                time.sleep(0.5)
                self.return_to_town()
                self.heal_up()
                # Return to farming area via waypoint
                if not self.travel_to_area(self.target_area):
                    print("    Could not return to farming area!")
                    return total_killed
                time.sleep(1)
                continue
            elif hp_pct < 60:
                print(f"    LOW HP ({hp_pct:.0f}%) — retreating to safe distance")
                px, py = self.get_position()
                self.teleport_to(px - 20, py - 20)
                time.sleep(1)

            teleport_count = 0  # reset after finding monsters

        return total_killed

    # Items worth picking up (by name substring, case-insensitive)
    PICKUP_NAMES = [
        "unique", "set",           # quality keywords in colored names
        "rune", "key",             # always valuable
        "charm", "jewel", "ring", "amulet",  # small valuables
        "essence", "token",        # PD2 materials
        "voidstone", "vision", "pandemonium", "talisman",
    ]

    # Items to skip picking up
    SKIP_NAMES = [
        "gold", "potion", "scroll", "arrow", "bolt",
        "tome", "ear",
    ]

    def _should_pickup(self, item_name):
        """Check if an item is worth picking up by name."""
        name = item_name.lower()
        # Remove color codes
        while "ÿc" in name:
            idx = name.find("ÿc")
            name = name[:idx] + name[idx+3:]

        # Skip junk
        if any(s in name for s in self.SKIP_NAMES):
            return False

        # Always pick up valuables
        if any(s in name for s in self.PICKUP_NAMES):
            return True

        # Pick up equipment (might be good)
        return True  # for now, pick up everything that isn't junk

    def pick_up_loot(self):
        """Pick up valuable items on the ground with verification."""
        units = self.mcp.call("get_nearby_units", {"max_distance": 30})
        items = [u for u in units.get("units", []) if u.get("type") == "item"]

        # Snapshot inventory before pickup
        inv_before = self.mcp.call("get_inventory")
        inv_ids_before = {i["unit_id"] for i in inv_before.get("items", [])
                         if i.get("storage") == "inventory"}

        picked = 0
        verified = 0
        for item in items[:8]:
            name = item.get("name", "")
            if not self._should_pickup(name):
                continue

            # Walk close to item first
            self.mcp.call("walk_to", {"x": item["position"]["x"], "y": item["position"]["y"]})
            time.sleep(0.5)

            self.mcp.call("pickup_item", {"item_id": item["unit_id"]})
            time.sleep(0.5)
            picked += 1

            # Verify item entered inventory
            inv_after = self.mcp.call("get_inventory")
            inv_ids_after = {i["unit_id"] for i in inv_after.get("items", [])
                            if i.get("storage") == "inventory"}
            new_items = inv_ids_after - inv_ids_before
            if new_items:
                verified += 1
                inv_ids_before = inv_ids_after
                print(f"    Picked up: {name} (verified)")
            else:
                print(f"    Picked up: {name} (unverified)")

        self.stats["items_picked"] += picked
        self.stats["items_verified"] += verified
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

    # Healer NPCs by act
    HEALER_NAMES = {
        "akara", "fara", "ormus", "jamella", "malah",
    }

    def heal_up(self):
        """Heal to full HP by interacting with town healer NPC.
        Also recovers corpse if defense is abnormally low."""
        ps = self.mcp.call("get_player_state")
        if ps.get("_error"):
            return

        hp = ps.get("hp", 0)
        max_hp = ps.get("max_hp", 1)
        hp_pct = hp / max_hp * 100 if max_hp > 0 else 100
        defense = ps.get("attack", {}).get("total_def", 0)

        # Check for corpse (no gear = very low defense)
        if ps.get("level", 0) > 50 and defense < 100:
            print("  Low defense — looking for corpse...")
            units = self.mcp.call("get_nearby_units", {"max_distance": 50})
            for u in units.get("units", []):
                if u.get("type") == "player" and u.get("dead"):
                    print(f"  Walking to corpse at ({u['position']['x']},{u['position']['y']})")
                    self.mcp.call("walk_to", {
                        "x": u["position"]["x"], "y": u["position"]["y"]})
                    time.sleep(3)
                    break

        if hp_pct > 80:
            return  # healthy enough

        print(f"  HP at {hp_pct:.0f}% — seeking healer...")

        # Try to find healer NPC (search wider range)
        for search_range in [50, 100, 200]:
            units = self.mcp.call("get_nearby_units", {"max_distance": search_range})
            for u in units.get("units", []):
                if u.get("type") != "monster":
                    continue
                name = u.get("name", "").lower()
                if name in self.HEALER_NAMES:
                    print(f"  Interacting with {u['name']}...")
                    result = self.mcp.call("interact_entity", {
                        "unit_id": u["unit_id"], "unit_type": 1,
                        "expected_panel": "trade", "timeout_ms": 10000})
                    print(f"  Interact result: {result.get('status', '?')}")
                    time.sleep(1)
                    self.mcp.call("close_panels")
                    time.sleep(0.5)

                    ps2 = self.mcp.call("get_player_state")
                    hp2 = ps2.get("hp", 0)
                    print(f"  HP: {hp} -> {hp2}")
                    return

        # Fallback: travel to Harrogath (Malah is always near spawn)
        print("  No healer found, traveling to Harrogath for Malah...")
        gs = self.get_state()
        if gs.get("area") != 109:
            if self.travel_to_area(109):
                time.sleep(1)
                # Try to find Malah in Harrogath
                for search_range in [50, 100, 200]:
                    units = self.mcp.call("get_nearby_units", {"max_distance": search_range})
                    for u in units.get("units", []):
                        if u.get("type") != "monster":
                            continue
                        name = u.get("name", "").lower()
                        if name in self.HEALER_NAMES:
                            print(f"  Walking to {u['name']}...")
                            self.mcp.call("walk_to", {
                                "x": u["position"]["x"], "y": u["position"]["y"]})
                            time.sleep(5)
                            self.mcp.call("interact_object", {
                                "unit_id": u["unit_id"], "unit_type": 1})
                            time.sleep(2)
                            self.mcp.call("close_panels")
                            time.sleep(0.5)
                            ps2 = self.mcp.call("get_player_state")
                            print(f"  HP: {hp} -> {ps2.get('hp', 0)}")
                            return

        # Last fallback: wait for auto-potion / regen
        print("  Waiting for auto-potion...")
        for i in range(15):
            time.sleep(2)
            hp_pct = self.get_hp_pct()
            if hp_pct > 70:
                print(f"  HP recovered to {hp_pct:.0f}%")
                return
        print(f"  HP at {self.get_hp_pct():.0f}% (may still be low)")

    def run_single(self, area_id, farm_duration=60):
        """Run a single farming loop."""
        self.stats["runs"] += 1
        run_start = time.time()
        print(f"\n=== Run {self.stats['runs']} ===")

        # Close any open panels (waypoint panel often stays open after launch)
        self.mcp.call("close_panels")
        time.sleep(0.5)

        # Heal up: go to Harrogath (Malah is close to spawn) if HP is low
        self.heal_up()

        # Pre-check: ensure character can move
        if not self.ensure_mobile():
            print("  Character is stuck, skipping run")
            return False

        # Close any open panels
        self.mcp.call("close_panels")
        time.sleep(0.5)

        # If we're in the wrong act, waypoint to the target act's town first
        gs = self.get_state()
        current_area = gs.get("area", 0)
        target_town = self._get_target_town(area_id)
        current_town = self._get_target_town(current_area)

        if current_town != target_town:
            print(f"  In wrong act (area {current_area}), waypointing to {WAYPOINT_AREAS.get(target_town, '?')}...")
            self.travel_to_area(target_town)

        # Step 1: Travel to farming area
        if not self.travel_to_area(area_id):
            return False

        # Step 2: Farm (teleport + kill + loot)
        print(f"  Farming for {farm_duration}s...")
        killed = self.scan_and_attack(max_duration=farm_duration)
        self.stats["kills"] += killed
        print(f"  Killed {killed} monsters")

        # Check if we're still in game (chicken may have exited)
        gs = self.get_state()
        if gs.get("state") != "in_game" or gs.get("_error"):
            print("  Game exited (chicken or crash)")
            return False

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

    def _get_target_town(self, area_id):
        """Get town area for a given farming area."""
        if area_id <= 39: return 1
        elif area_id <= 74: return 40
        elif area_id <= 102: return 75
        elif area_id <= 108: return 103
        else: return 109

    def print_stats(self):
        """Print farming statistics."""
        elapsed = time.time() - self.stats["start_time"]
        mins = elapsed / 60
        print(f"\n=== Farming Stats ===")
        print(f"  Runs:      {self.stats['runs']}")
        print(f"  Kills:     {self.stats['kills']}")
        print(f"  Items:     {self.stats['items_found']} found, {self.stats['items_vendored']} vendored")
        print(f"  Pickup:    {self.stats['items_picked']} attempted, {self.stats['items_verified']} verified")
        print(f"  Deaths:    {self.stats['deaths']}")
        print(f"  Time:      {mins:.1f} minutes")
        if self.stats["runs"] > 0:
            print(f"  Avg run:   {elapsed/self.stats['runs']:.0f}s")
        if mins > 0:
            print(f"  Kills/min: {self.stats['kills']/mins:.1f}")


def main():
    parser = argparse.ArgumentParser(description="Farming Loop")
    parser.add_argument("--area", type=int, default=129, help="Area ID to farm (default: 129=WSK L2)")
    parser.add_argument("--runs", type=int, default=3, help="Number of runs (default: 3)")
    parser.add_argument("--duration", type=int, default=60, help="Farm duration per run in seconds (default: 60)")
    parser.add_argument("--new-game", action="store_true", help="Create new game each run (resets monsters)")
    parser.add_argument("--rotate", action="store_true", help="Rotate through farming areas each run")
    parser.add_argument("--character", type=str, default="combustion", help="Character name for new game creation")
    parser.add_argument("--dry-run", action="store_true", help="Show plan only")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    farm = FarmingLoop(args.url)

    # Check game state
    gs = farm.get_state()
    if gs.get("state") != "in_game":
        print(f"ERROR: Not in game (state: {gs.get('state', '?')})")
        sys.exit(1)

    # Build area schedule
    if args.rotate:
        areas = [FARM_ROTATION[i % len(FARM_ROTATION)] for i in range(args.runs)]
        area_names = [WAYPOINT_AREAS.get(a, f"Area {a}") for a in areas]
        print(f"Rotation: {', '.join(area_names[:5])}{'...' if len(areas) > 5 else ''}")
    else:
        areas = [args.area] * args.runs

    area_name = WAYPOINT_AREAS.get(args.area, f"Area {args.area}")
    if not args.rotate:
        print(f"Farming: {area_name} (area {args.area})")
    print(f"Runs: {args.runs}, Duration: {args.duration}s per run")
    if args.new_game:
        print(f"New game each run: character={args.character}")

    if args.dry_run:
        print("\n[DRY RUN] Would farm:")
        for i, a in enumerate(areas[:5]):
            print(f"  Run {i+1}: {WAYPOINT_AREAS.get(a, f'Area {a}')}")
        if len(areas) > 5:
            print(f"  ... and {len(areas) - 5} more runs")
        print(f"  Each run: teleport + kill {args.duration}s + loot + vendor")
        if args.new_game:
            print(f"  New game between runs")
        return

    # Verify character
    ps = farm.mcp.call("get_player_state")
    char_name = ps.get("name", "?")
    char_level = ps.get("level", 0)
    print(f"Character: {char_name} (Level {char_level} {ps.get('class', '?')})")
    if char_level < 10:
        print("WARNING: Low-level character — farming may not be effective")

    # Detect skills (uses cached mapping for known characters)
    print("Detecting skills...")
    if not farm.detect_skills():
        print("WARNING: Could not auto-detect skills, using defaults")
        farm.SKILL_TELEPORT = 394
        farm.SKILL_COMBAT = 54

    for run in range(args.runs):
        area_id = areas[run]
        success = farm.run_single(area_id, farm_duration=args.duration)
        if not success:
            print("Run failed, stopping")
            break

        # Create new game for next run (resets monsters)
        if args.new_game and run < args.runs - 1:
            if not farm.create_new_game(args.character):
                print("Failed to create new game, stopping")
                break
            farm.detect_skills()

    farm.print_stats()


if __name__ == "__main__":
    main()
