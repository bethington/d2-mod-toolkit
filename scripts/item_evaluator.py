"""Item Evaluator — Score and classify items for keep/vendor decisions.

Uses get_item_stats MCP tool to read item affixes, then evaluates them
against configurable rules. Outputs keep/vendor/review recommendations.

Usage:
    python item_evaluator.py --scan-tab 1         # Evaluate all items on tab 1
    python item_evaluator.py --scan-all            # Evaluate entire stash
    python item_evaluator.py --item 680            # Evaluate a single item
    python item_evaluator.py --vendor-list         # Show items safe to vendor

Requires: game running with stash open, MCP server on port 21337.
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
        text = r.json().get("result", {}).get("content", [{}])[0].get("text", "")
        return json.loads(text) if text.startswith("{") else {}


# ---- Evaluation Rules ----

# Grand Charm skill tab sub_indices to KEEP (your Sorceress's useful trees)
# Set these to match your class. Check actual sub_indices by running --scan-tab.
# Common D2 skill tabs: Sorc Fire=3, Sorc Lightning=4, Sorc Cold=5
# PD2 may use different IDs — inspect your charms and update these.
KEEP_SKILL_TABS = set()  # Empty = keep ALL skillers (fill in after inspecting)

# Evaluation thresholds
RULES = {
    # Small Charms: minimum stat values to keep
    "small_charm": {
        "keep_if_any": [
            {"life": 17},               # +17 life or better
            {"fire_resist": 9},          # +9 fire resist or better
            {"cold_resist": 9},
            {"lightning_resist": 9},
            {"poison_resist": 9},
            {"life": 12, "resist_any": 5},  # dual mod: 12+ life AND 5+ any resist
            {"magic_find": 5},           # 5+ MF
            {"frw": 3},                  # 3+ FRW
            {"max_dmg": 3, "ar": 15},    # 3+ max damage AND 15+ AR (physical chars)
        ],
        "vendor_below": {
            "life": 10,      # vendor if ONLY stat is life < 10
        }
    },
    # Grand Charms: keep skillers, life skillers, vendor plain
    "grand_charm": {
        "keep_if_skiller": True,         # always keep +1 skill tree
        "keep_if_life_skiller": True,    # especially if +life secondary
        "min_life_secondary": 20,        # +20 life on a skiller = great
        "vendor_if_no_skiller": True,    # vendor non-skiller GCs
        "keep_if_any": [
            {"life": 30},                # +30+ life GC (no skiller needed)
        ]
    },
    # Large Charms: generally vendor unless exceptional
    "large_charm": {
        "keep_if_any": [
            {"life": 25},
            {"resist_any": 10},
        ],
        "vendor_default": True,
    },
    # Jewelry: keep anything identified with good stats
    "ring": {
        "keep_if_any": [
            {"fcr": 10},
            {"life_leech": 3},
            {"mana_leech": 3},
            {"all_skills": 1},
            {"resist_any": 20},
            {"life": 30},
        ]
    },
    "amulet": {
        "keep_if_any": [
            {"fcr": 10},
            {"all_skills": 1},
            {"single_skill": 2},
            {"resist_any": 20},
        ]
    },
    # Equipment: keep unique/set/rare with good rolls, vendor inferior/normal
    "equipment": {
        "auto_vendor_qualities": ["inferior", "normal"],
        "auto_keep_qualities": ["unique", "set"],
        "review_qualities": ["rare", "craft", "magic"],
    }
}

# Stat ID mappings for evaluation
STAT_MAP = {
    "life": [7],           # max_life (>>8 for display)
    "mana": [9],           # max_mana (>>8)
    "fire_resist": [39],
    "cold_resist": [43],
    "lightning_resist": [41],
    "poison_resist": [45],
    "defense": [31],
    "strength": [0],
    "dexterity": [2],
    "vitality": [3],
    "energy": [1],
    "fcr": [105],
    "fhr": [99],
    "frw": [96],
    "ias": [93],
    "all_skills": [127],
    "single_skill": [107],
    "life_leech": [60],
    "mana_leech": [62],
    "magic_find": [80],
    "gold_find": [79],
    "crushing_blow": [136],
    "open_wounds": [135],
    "deadly_strike": [141],
    "max_dmg": [22],
    "min_dmg": [21],
    "ar": [19],            # attack rating
    "enhanced_damage": [25],
    "enhanced_defense": [16],
    "sockets": [214],
}


def get_stat_value(stats, stat_name):
    """Get a stat value from the stats list by name."""
    ids = STAT_MAP.get(stat_name, [])
    for s in stats:
        if s["id"] in ids:
            v = s.get("display_value", s["value"])
            # Life/mana need >>8 if not already display_value
            if stat_name in ("life", "mana") and "display_value" not in s:
                v = s["value"] >> 8
            return v
    return 0


def get_any_resist(stats):
    """Get the highest single resist value."""
    return max(
        get_stat_value(stats, "fire_resist"),
        get_stat_value(stats, "cold_resist"),
        get_stat_value(stats, "lightning_resist"),
        get_stat_value(stats, "poison_resist"),
    )


def has_skiller(stats):
    """Check if item has +1 skill tree (stat 188)."""
    return any(s["id"] == 188 for s in stats)


def get_skiller_tab(stats):
    """Get the skill tab sub_index if item is a skiller."""
    for s in stats:
        if s["id"] == 188:
            return s.get("sub_index", -1)
    return -1


def check_threshold(stats, conditions):
    """Check if stats meet a set of threshold conditions."""
    for key, threshold in conditions.items():
        if key == "resist_any":
            if get_any_resist(stats) < threshold:
                return False
        elif key == "ar":
            if get_stat_value(stats, "ar") < threshold:
                return False
        else:
            if get_stat_value(stats, key) < threshold:
                return False
    return True


# ---- Evaluator ----

class ItemEvaluator:
    def __init__(self, mcp_url="http://127.0.0.1:21337"):
        self.mcp = McpClient(mcp_url)

    def evaluate_item(self, item_id):
        """Evaluate a single item. Returns (verdict, reason, item_data)."""
        data = self.mcp.call("get_item_stats", {"item_id": item_id})
        if not data or "_is_error" in data:
            return ("error", "Could not read item", {})

        name = data.get("name", "?").lower()
        quality = data.get("quality", "unknown")
        stats = data.get("stats", [])
        identified = data.get("identified", False)

        # Strip color codes for matching
        clean_name = name
        while "\xc3\xbf" in clean_name or "ÿc" in clean_name:
            idx = clean_name.find("ÿc")
            if idx >= 0:
                clean_name = clean_name[:idx] + clean_name[idx+3:]
            else:
                break

        # Determine item category
        if "small charm" in clean_name:
            return self._eval_small_charm(data, stats)
        elif "grand charm" in clean_name:
            return self._eval_grand_charm(data, stats)
        elif "large charm" in clean_name:
            return self._eval_large_charm(data, stats)
        elif "ring" in clean_name:
            return self._eval_jewelry(data, stats, "ring")
        elif "amulet" in clean_name:
            return self._eval_jewelry(data, stats, "amulet")
        elif "jewel" in clean_name:
            return self._eval_jewel(data, stats)
        else:
            return self._eval_equipment(data, stats)

    def _eval_small_charm(self, data, stats):
        rules = RULES["small_charm"]
        for conditions in rules["keep_if_any"]:
            if check_threshold(stats, conditions):
                return ("keep", self._describe_stats(stats), data)

        life = get_stat_value(stats, "life")
        if life > 0 and life < rules["vendor_below"]["life"] and len(stats) <= 2:
            return ("vendor", f"low life only ({life})", data)

        if not stats:
            return ("vendor", "no stats", data)

        return ("review", self._describe_stats(stats), data)

    def _eval_grand_charm(self, data, stats):
        rules = RULES["grand_charm"]
        is_skiller = has_skiller(stats)
        tab = get_skiller_tab(stats)
        life = get_stat_value(stats, "life")

        if is_skiller:
            if KEEP_SKILL_TABS and tab not in KEEP_SKILL_TABS:
                return ("vendor", f"wrong class skiller (tab {tab})", data)
            if life >= rules["min_life_secondary"]:
                return ("keep", f"+1 skill tab {tab} +{life} life (great!)", data)
            return ("keep", f"+1 skill tab {tab}" + (f" +{life} life" if life else ""), data)

        # Non-skiller GC
        for conditions in rules.get("keep_if_any", []):
            if check_threshold(stats, conditions):
                return ("keep", self._describe_stats(stats), data)

        if rules.get("vendor_if_no_skiller"):
            return ("vendor", f"no skiller ({self._describe_stats(stats) or 'plain'})", data)

        return ("review", self._describe_stats(stats), data)

    def _eval_large_charm(self, data, stats):
        rules = RULES["large_charm"]
        for conditions in rules["keep_if_any"]:
            if check_threshold(stats, conditions):
                return ("keep", self._describe_stats(stats), data)
        if rules.get("vendor_default"):
            return ("vendor", self._describe_stats(stats) or "plain", data)
        return ("review", self._describe_stats(stats), data)

    def _eval_jewelry(self, data, stats, jtype):
        rules = RULES.get(jtype, {})
        quality = data.get("quality", "unknown")

        if not data.get("identified"):
            return ("review", "unidentified", data)

        for conditions in rules.get("keep_if_any", []):
            if check_threshold(stats, conditions):
                return ("keep", self._describe_stats(stats), data)

        if quality in ("unique", "set"):
            return ("keep", f"{quality} {self._describe_stats(stats)}", data)

        return ("review", self._describe_stats(stats), data)

    def _eval_jewel(self, data, stats):
        if not data.get("identified"):
            return ("review", "unidentified", data)

        # Keep jewels with good stats
        has_good = False
        for key in ["ias", "enhanced_damage", "fire_resist", "cold_resist",
                     "lightning_resist", "poison_resist", "max_dmg", "all_skills"]:
            if get_stat_value(stats, key) > 0:
                has_good = True
                break

        if has_good:
            return ("review", self._describe_stats(stats), data)
        return ("vendor", self._describe_stats(stats) or "plain", data)

    def _eval_equipment(self, data, stats):
        rules = RULES["equipment"]
        quality = data.get("quality", "unknown")

        if quality in rules.get("auto_vendor_qualities", []):
            return ("vendor", f"{quality} quality", data)
        if quality in rules.get("auto_keep_qualities", []):
            return ("keep", f"{quality} {self._describe_stats(stats)}", data)

        # Rare/craft: check for good stats
        has_fcr = get_stat_value(stats, "fcr") > 0
        has_resist = get_any_resist(stats) >= 20
        has_life = get_stat_value(stats, "life") >= 20
        has_skills = get_stat_value(stats, "all_skills") > 0
        sockets = data.get("sockets", 0)

        if has_skills or (has_fcr and has_resist) or sockets >= 3:
            return ("keep", self._describe_stats(stats), data)

        if quality in ("rare", "craft"):
            return ("review", self._describe_stats(stats), data)

        return ("vendor", self._describe_stats(stats) or quality, data)

    def _describe_stats(self, stats, max_show=5):
        """Build a compact stat description string."""
        parts = []
        for s in stats[:max_show]:
            name = s.get("name", "?")
            val = s.get("display_value", s.get("value", 0))
            # Shorten common names
            short = {
                "max_life": "life", "fire_resist": "fRes", "cold_resist": "cRes",
                "lightning_resist": "lRes", "poison_resist": "pRes",
                "enhanced_defense": "eDef", "enhanced_damage": "eDmg",
                "skill_on_attack": "skiller", "single_skill": "+skill",
                "attack_rating": "AR", "magic_find": "MF", "gold_find": "GF",
            }.get(name, name)
            sub = s.get("sub_index", "")
            if sub and name in ("skill_on_attack", "single_skill", "skill_tab"):
                parts.append(f"{short}[{sub}]={val}")
            else:
                parts.append(f"{short}={val}")
        return "  ".join(parts)

    def scan_tab(self, tab):
        """Evaluate all items on a stash tab."""
        self.mcp.call("switch_stash_tab", {"tab": tab})
        time.sleep(0.5)

        # Use the stash grid to find which item IDs are on THIS tab
        grid_data = self.mcp.call("get_stash_grid", {"container": "stash"})
        grid = grid_data.get("grid", [])
        tab_uids = set()
        for row in grid:
            for uid in row:
                if uid != 0:
                    tab_uids.add(uid)

        results = {"keep": [], "vendor": [], "review": []}

        for uid in tab_uids:
            verdict, reason, data = self.evaluate_item(uid)
            entry = {
                "unit_id": uid,
                "name": data.get("name", "?"),
                "quality": data.get("quality", "?"),
                "reason": reason,
            }
            results[verdict].append(entry)

        return results

    def print_results(self, results, tab=None):
        """Print evaluation results."""
        tab_str = f" (Tab {tab})" if tab is not None else ""

        keep = results.get("keep", [])
        vendor = results.get("vendor", [])
        review = results.get("review", [])

        print(f"\n=== EVALUATION{tab_str}: {len(keep)} keep, {len(vendor)} vendor, {len(review)} review ===\n")

        if keep:
            print("KEEP:")
            for item in keep:
                print(f"  [{item['unit_id']:5d}] {item['name'][:30]:30s} {item['quality']:8s}  {item['reason']}")

        if vendor:
            print("\nVENDOR:")
            for item in vendor:
                print(f"  [{item['unit_id']:5d}] {item['name'][:30]:30s} {item['quality']:8s}  {item['reason']}")

        if review:
            print("\nREVIEW (manual check):")
            for item in review:
                print(f"  [{item['unit_id']:5d}] {item['name'][:30]:30s} {item['quality']:8s}  {item['reason']}")


# ---- CLI ----

def main():
    parser = argparse.ArgumentParser(description="Item Evaluator")
    parser.add_argument("--scan-tab", type=int, help="Evaluate all items on a tab")
    parser.add_argument("--scan-all", action="store_true", help="Evaluate entire stash")
    parser.add_argument("--item", type=int, help="Evaluate a single item by ID")
    parser.add_argument("--vendor-list", action="store_true", help="Show only vendor candidates")
    parser.add_argument("--url", default="http://127.0.0.1:21337")
    args = parser.parse_args()

    ev = ItemEvaluator(args.url)

    if args.item:
        verdict, reason, data = ev.evaluate_item(args.item)
        name = data.get("name", "?")
        quality = data.get("quality", "?")
        print(f"Item {args.item}: {name} ({quality})")
        print(f"  Verdict: {verdict.upper()}")
        print(f"  Reason: {reason}")
        stats = data.get("stats", [])
        if stats:
            print(f"  Stats ({len(stats)}):")
            for s in stats:
                n = s.get("name", "?")
                v = s.get("display_value", s.get("value", 0))
                si = s.get("sub_index", "")
                print(f"    {n} = {v}" + (f" [sub={si}]" if si else ""))
        return

    if args.scan_tab is not None:
        results = ev.scan_tab(args.scan_tab)
        ev.print_results(results, tab=args.scan_tab)
        return

    if args.scan_all or args.vendor_list:
        all_vendor = []
        for tab in range(1, 10):
            print(f"Scanning tab {tab}...", end=" ", flush=True)
            results = ev.scan_tab(tab)
            print(f"{len(results['keep'])} keep, {len(results['vendor'])} vendor, {len(results['review'])} review")
            if args.vendor_list:
                for v in results["vendor"]:
                    v["tab"] = tab
                    all_vendor.append(v)
            else:
                ev.print_results(results, tab=tab)

        if args.vendor_list and all_vendor:
            print(f"\n=== VENDOR LIST ({len(all_vendor)} items) ===\n")
            for item in all_vendor:
                print(f"  Tab {item['tab']} [{item['unit_id']:5d}] {item['name'][:30]:30s} {item['quality']:8s}  {item['reason']}")
        return

    parser.print_help()


if __name__ == "__main__":
    main()
