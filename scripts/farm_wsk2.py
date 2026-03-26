"""WSK2 farming loop - teleport, combustion, chicken, repeat."""
import json, time, requests, sys

MCP = "http://127.0.0.1:21337/mcp"
TELE = 54       # Teleport skill ID
COMBAT = 376    # Combustion skill ID
MERC_IDS = {271, 338, 359, 560, 561}
CHICKEN_PCT = 10   # Only chicken at truly lethal HP
FLEE_PCT = 45
HEAL_PCT = 50      # Go heal at Malah when below this

def mcp(tool, args=None, timeout=10):
    try:
        r = requests.post(MCP, json={
            "jsonrpc": "2.0", "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}
        }, timeout=timeout)
        data = r.json()
        return json.loads(data["result"]["content"][0]["text"])
    except Exception as e:
        return {"_error": str(e)}

def player():
    return mcp("get_player_state")

def cast(skill_id, x, y):
    mcp("switch_skill", {"skill_id": skill_id, "left": False})
    time.sleep(0.15)
    mcp("cast_skill", {"x": x, "y": y, "left": False})
    time.sleep(0.35)

def teleport(x, y):
    cast(TELE, x, y)

def attack(x, y, times=6):
    mcp("switch_skill", {"skill_id": COMBAT, "left": False})
    time.sleep(0.1)
    for _ in range(times):
        mcp("cast_skill", {"x": x, "y": y, "left": False})
        time.sleep(0.2)

def get_targets(max_dist=25):
    data = mcp("get_nearby_units", {"max_distance": max_dist})
    units = data.get("units", [])
    alive = [u for u in units if not u.get("dead") and u.get("hp", 0) > 0
             and u.get("class_id") not in MERC_IDS]
    hittable = [u for u in alive if u.get("resistances", {}).get("fire", 0) < 100]
    immune = [u for u in alive if u.get("resistances", {}).get("fire", 0) >= 100]
    return hittable, immune

def heal_at_malah():
    """TP to town, talk to Malah to heal, then TP back."""
    print("  Healing at Malah...")
    # Use a TP scroll/tome — find one in inventory
    inv = mcp("get_inventory")
    tp_item = None
    for item in inv.get("items", []):
        name = item.get("name", "").lower()
        if "scroll of town portal" in name or "tome of town portal" in name:
            tp_item = item
            break

    if tp_item:
        mcp("use_item", {"item_id": tp_item["unit_id"]})
        time.sleep(2)
        # Walk to Malah — she's an NPC in Harrogath
        objs = mcp("get_nearby_units", {"max_distance": 100})
        for u in objs.get("units", []):
            if "Malah" in u.get("name", ""):
                result = mcp("interact_entity", {
                    "unit_id": u["unit_id"], "unit_type": 1,
                    "expected_panel": "trade", "timeout_ms": 10000
                })
                if result.get("status") == "opened":
                    mcp("close_panels")
                    time.sleep(0.5)
                print(f"  Healed! HP now full")
                break
        # Go back through portal
        time.sleep(1)
        objs = mcp("get_nearby_objects", {"max_distance": 50})
        for o in objs.get("objects", []):
            if "Portal" in o.get("name", ""):
                mcp("interact_entity", {
                    "unit_id": o["unit_id"], "unit_type": 2,
                    "timeout_ms": 10000
                })
                time.sleep(2)
                break
        ps = player()
        print(f"  Back in {ps.get('area_name','?')} HP={ps['hp']}/{ps['max_hp']}")
    else:
        print("  No TP scroll/tome found! Teleporting to safety instead.")
        ps = player()
        teleport(ps["position"]["x"] + 40, ps["position"]["y"] - 40)

def walk_to_waypoint():
    """Find and walk to the Harrogath waypoint."""
    for attempt in range(3):
        objs = mcp("get_nearby_objects", {"max_distance": 200})
        for o in objs.get("objects", []):
            if "Waypoint" in o.get("name", ""):
                wp_id = o["unit_id"]
                wp_x, wp_y = o["position"]["x"], o["position"]["y"]
                print(f"  WP id={wp_id} at ({wp_x},{wp_y}) dist={o['distance']}")

                # Walk step by step
                for _ in range(10):
                    ps = player()
                    cx, cy = ps["position"]["x"], ps["position"]["y"]
                    dx, dy = wp_x - cx, wp_y - cy
                    if abs(dx) + abs(dy) < 6:
                        break
                    sx = max(-5, min(5, dx))
                    sy = max(-5, min(5, dy))
                    mcp("walk_to", {"x": cx + sx, "y": cy + sy, "run": True})
                    time.sleep(0.8)

                # Interact
                result = mcp("interact_entity", {
                    "unit_id": wp_id, "unit_type": 2,
                    "expected_panel": "waypoint", "timeout_ms": 10000
                })
                if result.get("status") == "opened":
                    return wp_id
                print(f"  WP interact failed: {result.get('status')}")
        time.sleep(1)
    return None

def travel_to_wsk2(wp_id):
    """Use waypoint to travel to WSK2."""
    mcp("use_waypoint", {"waypoint_id": wp_id, "destination": 129})
    time.sleep(3)
    mcp("close_panels")
    time.sleep(0.5)
    ps = player()
    area = ps.get("area_name", "")
    print(f"  Area: {area}")
    return "Worldstone" in area

def farm_loop(max_rounds=30):
    """Teleport around WSK2, kill non-fire-immune monsters."""
    kills = 0

    # Immediate safety teleport on arrival — get away from WP spawn
    ps = player()
    cx, cy = ps["position"]["x"], ps["position"]["y"]
    print(f"  Safety teleport from ({cx},{cy})...")
    for _ in range(3):
        teleport(cx + 35, cy + 25)
        time.sleep(0.3)
        ps = player()
        cx, cy = ps["position"]["x"], ps["position"]["y"]
    print(f"  Now at ({cx},{cy})")

    for rnd in range(1, max_rounds + 1):
        ps = player()
        if "_error" in ps:
            print(f"  MCP error: {ps['_error']}")
            break

        cx, cy = ps["position"]["x"], ps["position"]["y"]
        hp_pct = ps["hp"] * 100 // ps["max_hp"] if ps["max_hp"] > 0 else 100

        # Chicken only at truly lethal HP
        if hp_pct < CHICKEN_PCT:
            print(f"  CHICKEN at {hp_pct}%!")
            mcp("update_stream", {"chicken": True, "event": f"Chicken at {hp_pct}% HP"})
            mcp("quit_game")
            return kills, "chicken"

        # Heal at Malah when HP is low
        if hp_pct < HEAL_PCT:
            print(f"  [{rnd}] HP={hp_pct}% — healing at Malah")
            # Teleport away from danger first
            teleport(cx + 35, cy - 35)
            time.sleep(0.3)
            heal_at_malah()
            continue

        # Teleport in golden-angle spiral
        angle = (rnd * 137) % 360
        dx = (1 if angle < 180 else -1) * 30
        dy = (1 if 90 < angle < 270 else -1) * 25
        teleport(cx + dx, cy + dy)

        # Scan
        hittable, immune = get_targets()

        if hittable:
            # Attack center of mass
            ax = sum(u["position"]["x"] for u in hittable) // len(hittable)
            ay = sum(u["position"]["y"] for u in hittable) // len(hittable)
            names = ", ".join(set(u["name"] for u in hittable[:3]))
            print(f"  [{rnd}] FIGHT {len(hittable)}x {names} HP={hp_pct}%")

            attack(ax, ay, times=8)

            # Count dead
            h2, _ = get_targets()
            new_kills = len(hittable) - len(h2)
            if new_kills > 0:
                kills += new_kills
                mcp("update_stream", {"kills": new_kills})
                print(f"    -> {new_kills} killed (total: {kills})")
            else:
                print(f"    -> 0 killed, {len(h2)} remain")
        elif immune:
            print(f"  [{rnd}] {len(immune)} fire immunes — skip")
        else:
            pass  # Clear, keep moving

    return kills, "done"

def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    total_kills = 0

    for run in range(1, runs + 1):
        print(f"\n=== RUN {run}/{runs} ===")

        # Setup
        mcp("set_auto_potion", {"enabled": True, "hp_threshold": 50, "mp_threshold": 30, "rejuv_threshold": 25})
        mcp("reveal_map")
        mcp("update_stream", {"status": f"WSK2 Run {run}/{runs}", "event": f"Starting run {run}"})

        # Pre-run: heal if needed
        ps = player()
        hp_pct = ps["hp"] * 100 // ps["max_hp"] if ps["max_hp"] > 0 else 100
        if hp_pct < 90:
            print(f"  HP at {hp_pct}% — healing at Malah before farming")
            heal_at_malah()

        # Find and use waypoint
        print("Finding waypoint...")
        wp_id = walk_to_waypoint()
        if wp_id is None:
            print("ERROR: Could not find/open waypoint!")
            break

        # Travel to WSK2
        print("Traveling to WSK2...")
        if not travel_to_wsk2(wp_id):
            print("ERROR: Failed to reach WSK2!")
            break

        # Farm
        print("Farming...")
        kills, reason = farm_loop(max_rounds=30)
        total_kills += kills
        print(f"  Run {run} done: {kills} kills, reason={reason}")

        mcp("update_stream", {"run_complete": True, "status": f"Run {run} done — {kills} kills"})

        if reason == "chicken":
            print("  Restarting after chicken...")
            time.sleep(5)
            # Check if game is still running
            try:
                mcp("ping", timeout=3)
                # Still up — we're at menu, need to re-enter
            except:
                print("  Game died, need relaunch")
                break

    print(f"\n=== SESSION TOTAL: {total_kills} kills across {runs} runs ===")

if __name__ == "__main__":
    main()
