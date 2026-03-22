"""Autonomous struct discovery for D2 game memory.

Connects to d2-mod-toolkit MCP and systematically explores game memory
starting from known anchor points, documenting everything it finds.

This script is designed to be called by Claude Code to build up
the struct registry automatically.

Usage:
    python auto_discover.py --output structs/
    python auto_discover.py --anchor PlayerUnit
    python auto_discover.py --full-scan
"""

import json
import os
import sys
import time
from datetime import datetime
from collections import OrderedDict

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


class McpClient:
    def __init__(self, url):
        self.url = url.rstrip("/")
        self._id = 0

    def call(self, tool, arguments=None):
        self._id += 1
        payload = {
            "jsonrpc": "2.0", "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": arguments or {}}
        }
        try:
            r = requests.post(f"{self.url}/mcp", json=payload, timeout=10)
            data = r.json()
            if "result" in data and "content" in data["result"]:
                text = data["result"]["content"][0].get("text", "")
                return json.loads(text) if text.startswith(("{", "[")) else {"text": text}
            return data
        except Exception as e:
            return {"error": str(e)}

    def alive(self):
        try:
            return requests.get(f"{self.url}/health", timeout=2).status_code == 200
        except:
            return False


class AutoDiscoverer:
    """Systematically discover and document game memory structures."""

    # Known anchor points — addresses of global pointers we can start from
    # These are resolved at runtime via the game's D2CLIENT module
    ANCHORS = {
        "PlayerUnit": {"dll": "D2CLIENT", "offset": 0x11BBFC, "type": "UnitAny"},
        "PlayerUnitList": {"dll": "D2CLIENT", "offset": 0x11BC14, "type": "RosterUnit"},
        "FirstControl": {"dll": "D2WIN", "offset": 0x214A0, "type": "Control"},
        "GameInfo": {"dll": "D2CLIENT", "offset": 0x11B980, "type": "GameStructInfo"},
        "sgptDataTable": {"dll": "D2COMMON", "offset": 0x99E1C, "type": "sgptDataTable"},
    }

    # Known DLL base addresses for PD2 1.13c
    DLL_BASES = {
        "D2CLIENT": 0x6FAB0000,
        "D2COMMON": 0x6FD50000,
        "D2WIN": 0x6F8E0000,
        "D2GFX": 0x6FA80000,
        "D2GAME": 0x6FC20000,
        "D2NET": 0x6FBF0000,
        "D2LANG": 0x6FC00000,
        "D2LAUNCH": 0x6FA40000,
        "FOG": 0x6FF50000,
        "STORM": 0x6FBF0000,
    }

    def __init__(self, game_url="http://127.0.0.1:21337", output_dir="structs"):
        self.game = McpClient(game_url)
        self.output_dir = output_dir
        self.discovered = {}  # struct_name -> {fields, instances, confidence}
        self.explored_addresses = set()

    def resolve_anchor(self, name):
        """Resolve an anchor point to a live address."""
        anchor = self.ANCHORS.get(name)
        if not anchor:
            return None

        base = self.DLL_BASES.get(anchor["dll"], 0)
        ptr_addr = base + anchor["offset"]

        result = self.game.call("read_memory", {
            "address": f"0x{ptr_addr:08X}",
            "size": 4,
            "format": "dwords"
        })

        if "error" in result:
            return None

        dwords = result.get("dwords", [])
        if not dwords:
            return None

        addr = dwords[0].get("value", 0)
        if addr == 0 or addr == 0xFFFFFFFF:
            return None

        return f"0x{addr:08X}"

    def explore_anchor(self, name, depth=2):
        """Explore from a known anchor point."""
        anchor = self.ANCHORS.get(name)
        if not anchor:
            print(f"Unknown anchor: {name}")
            return None

        addr = self.resolve_anchor(name)
        if not addr:
            print(f"Could not resolve anchor: {name}")
            return None

        print(f"\n{'='*60}")
        print(f"Exploring {name} at {addr} (type: {anchor['type']})")
        print(f"{'='*60}")

        return self._explore_typed(addr, anchor["type"], depth, name)

    def _explore_typed(self, address, struct_type, depth, context=""):
        """Explore an address as a known struct type."""
        if address in self.explored_addresses:
            return {"skipped": "already explored"}
        self.explored_addresses.add(address)

        result = self.game.call("read_struct", {
            "address": address,
            "struct_name": struct_type,
            "follow_pointers": False
        })

        if "error" in result:
            # Try discovering it as unknown
            return self._discover_unknown(address, 128, context)

        print(f"\n  {struct_type} at {address}:")
        fields_summary = []

        for f in result.get("fields", []):
            val = f.get("value", "?")
            name = f["name"]
            pts = f.get("points_to", "")

            # Print non-null, non-padding fields
            if f.get("hex"):
                print(f"    +0x{f['offset']:02X} {name:20s} = {f['hex']} ({val})")
            elif isinstance(val, str) and val.startswith("0x"):
                is_null = f.get("is_null", val == "0x00000000")
                if not is_null and pts:
                    print(f"    +0x{f['offset']:02X} {name:20s} = {val} -> {pts}")
                elif not is_null:
                    print(f"    +0x{f['offset']:02X} {name:20s} = {val}")
            else:
                print(f"    +0x{f['offset']:02X} {name:20s} = {val}")

            fields_summary.append({
                "name": name, "offset": f["offset"],
                "value": val, "points_to": pts
            })

            # Follow pointers if we have depth
            if (depth > 0 and pts and
                isinstance(val, str) and val.startswith("0x") and
                val != "0x00000000" and
                val not in self.explored_addresses):

                # Check if we have a struct def for the target
                sdef = self.game.call("get_struct_def", {"name": pts})
                if "error" not in sdef:
                    self._explore_typed(val, pts, depth - 1, f"{context}.{name}")

        return {"type": struct_type, "address": address, "fields": fields_summary}

    def _discover_unknown(self, address, size, context=""):
        """Discover an unknown struct at an address."""
        print(f"\n  [UNKNOWN] at {address} (context: {context}), analyzing {size} bytes...")

        result = self.game.call("read_region", {
            "address": address,
            "size": size
        })

        if "error" in result:
            print(f"    Cannot read: {result['error']}")
            return None

        pointers = 0
        zeros = 0
        for dw in result.get("dwords", []):
            if dw["type"] == "pointer":
                pointers += 1
                print(f"    +0x{dw['offset']:02X}  PTR  {dw['hex']}")
            elif dw["type"] == "zero":
                zeros += 1
            elif dw["type"] == "small_int":
                print(f"    +0x{dw['offset']:02X}  INT  {dw['hex']} ({dw['value']})")

        print(f"    Summary: {pointers} pointers, {zeros} zeros, {len(result.get('dwords',[]))} total DWORDs")
        return {"address": address, "pointers": pointers, "zeros": zeros}

    def full_scan(self, depth=2):
        """Scan all known anchor points."""
        results = {}
        for name in self.ANCHORS:
            try:
                r = self.explore_anchor(name, depth)
                if r:
                    results[name] = r
            except Exception as e:
                print(f"Error exploring {name}: {e}")
        return results

    def save_results(self):
        """Save all discovered data to the output directory."""
        os.makedirs(self.output_dir, exist_ok=True)

        # Save exploration log
        log = {
            "timestamp": datetime.now().isoformat(),
            "explored_count": len(self.explored_addresses),
            "explored_addresses": list(self.explored_addresses),
        }
        log_path = os.path.join(self.output_dir, "discovery_log.json")
        with open(log_path, "w") as f:
            json.dump(log, f, indent=2, default=str)
        print(f"\nSaved discovery log to {log_path}")

        # Export struct definitions
        structs = self.game.call("list_struct_defs")
        all_defs = {}
        for s in structs.get("structs", []):
            sdef = self.game.call("get_struct_def", {"name": s["name"]})
            if "error" not in sdef:
                all_defs[s["name"]] = sdef

        defs_path = os.path.join(self.output_dir, "structs.json")
        with open(defs_path, "w") as f:
            json.dump(all_defs, f, indent=2)
        print(f"Saved {len(all_defs)} struct definitions to {defs_path}")

        # Generate C header
        header_path = os.path.join(self.output_dir, "D2Structs_discovered.h")
        self._generate_header(all_defs, header_path)

    def _generate_header(self, structs, path):
        """Generate C header from struct definitions."""
        type_map = {
            "byte": "BYTE", "word": "WORD", "dword": "DWORD",
            "int": "int", "float": "float", "pointer": "void*",
            "string": "char", "wstring": "wchar_t", "padding": "BYTE"
        }

        with open(path, "w") as f:
            f.write("// Auto-generated by d2-mod-toolkit auto_discover.py\n")
            f.write(f"// Generated: {datetime.now().isoformat()}\n")
            f.write(f"// Structs: {len(structs)}\n")
            f.write("#pragma once\n#include <windows.h>\n\n")

            # Forward declarations
            for name in sorted(structs.keys()):
                f.write(f"struct {name};\n")
            f.write("\n")

            for name in sorted(structs.keys()):
                sdef = structs[name]
                f.write(f"struct {name} {{ // size 0x{sdef.get('size', 0):X}\n")
                for field in sdef.get("fields", []):
                    ft = field.get("type", "padding")
                    ct = type_map.get(ft, "DWORD")
                    fname = field["name"]
                    offset = field["offset"]
                    size = field.get("size", 4)
                    comment = field.get("comment", "")
                    pts = field.get("points_to", "")

                    if ft == "pointer" and pts:
                        f.write(f"    {pts}* {fname};")
                    elif ft == "string":
                        f.write(f"    char {fname}[{size}];")
                    elif ft == "padding":
                        f.write(f"    BYTE {fname}[{size}];")
                    else:
                        f.write(f"    {ct} {fname};")

                    f.write(f" // 0x{offset:02X}")
                    if comment:
                        f.write(f" {comment}")
                    f.write("\n")

                f.write(f"}}; // 0x{sdef.get('size', 0):X}\n\n")

        print(f"Generated C header: {path}")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Auto-discover D2 memory structures")
    parser.add_argument("--game-url", default="http://127.0.0.1:21337")
    parser.add_argument("--output", default="structs")
    parser.add_argument("--anchor", help="Explore a specific anchor (e.g., PlayerUnit)")
    parser.add_argument("--full-scan", action="store_true", help="Scan all known anchors")
    parser.add_argument("--depth", type=int, default=2)
    args = parser.parse_args()

    discoverer = AutoDiscoverer(args.game_url, args.output)

    if not discoverer.game.alive():
        print("ERROR: Game MCP not responding")
        sys.exit(1)

    if args.anchor:
        discoverer.explore_anchor(args.anchor, args.depth)
    elif args.full_scan:
        discoverer.full_scan(args.depth)
    else:
        # Default: explore PlayerUnit
        discoverer.explore_anchor("PlayerUnit", args.depth)

    discoverer.save_results()


if __name__ == "__main__":
    main()
