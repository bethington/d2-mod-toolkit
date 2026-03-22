"""Smart struct explorer for D2 game memory.

Connects to:
- d2-mod-toolkit MCP server (port 21337) for live memory reads
- Ghidra MCP server (port 8089) for static analysis cross-reference

Features:
- explore: Smart traversal from a known pointer, follows interesting branches
- discover: Analyze unknown memory region, propose struct layout
- compare: Diff Ghidra definitions vs runtime memory
- export: Generate C headers + JSON from discovered structs

Usage:
    python struct_explorer.py explore --address 0x26158A00 --type UnitAny --depth 3
    python struct_explorer.py discover --address 0x26158A00 --size 256
    python struct_explorer.py compare --type UnitAny
    python struct_explorer.py export --output structs/
"""

import argparse
import json
import sys
import os
from typing import Optional, Dict, List, Any
from dataclasses import dataclass, field
from datetime import datetime

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


# ---- MCP Client ----

class McpClient:
    """Simple MCP client that talks to an HTTP+SSE MCP server."""

    def __init__(self, url: str):
        self.url = url.rstrip("/")
        self._id = 0

    def call(self, tool: str, arguments: dict = None) -> dict:
        self._id += 1
        payload = {
            "jsonrpc": "2.0",
            "id": self._id,
            "method": "tools/call",
            "params": {"name": tool, "arguments": arguments or {}}
        }
        try:
            r = requests.post(f"{self.url}/mcp", json=payload, timeout=10)
            data = r.json()
            if "result" in data and "content" in data["result"]:
                text = data["result"]["content"][0].get("text", "")
                return json.loads(text) if text.startswith("{") or text.startswith("[") else {"text": text}
            if "error" in data:
                return {"error": data["error"]}
            return data
        except Exception as e:
            return {"error": str(e)}

    def is_alive(self) -> bool:
        try:
            r = requests.get(f"{self.url}/health", timeout=2)
            return r.status_code == 200
        except:
            return False


# ---- Struct Explorer ----

@dataclass
class ExploredField:
    name: str
    offset: int
    type: str
    value: Any
    hex_value: str = ""
    comment: str = ""
    points_to: str = ""
    is_null: bool = False
    children: List["ExploredField"] = field(default_factory=list)
    ghidra_name: str = ""  # name from Ghidra if different
    confidence: str = "verified"  # verified, likely, unknown


@dataclass
class ExploredStruct:
    name: str
    address: str
    fields: List[ExploredField]
    size: int = 0
    source: str = ""


class StructExplorer:
    def __init__(self, game_url: str = "http://127.0.0.1:21337",
                 ghidra_url: str = "http://127.0.0.1:8089"):
        self.game = McpClient(game_url)
        self.ghidra_url = ghidra_url
        self._visited = set()  # prevent circular traversal

    def explore(self, address: str, struct_name: str, depth: int = 2,
                follow_nulls: bool = False) -> Optional[ExploredStruct]:
        """Smart traversal from a known pointer."""
        if address in self._visited:
            return None
        self._visited.add(address)

        # Read struct from game memory
        result = self.game.call("read_struct", {
            "address": address,
            "struct_name": struct_name,
            "follow_pointers": False
        })

        if "error" in result:
            print(f"  Error reading {struct_name} at {address}: {result['error']}")
            return None

        explored = ExploredStruct(
            name=struct_name,
            address=address,
            fields=[],
            size=result.get("size", 0),
            source="runtime"
        )

        for f in result.get("fields", []):
            ef = ExploredField(
                name=f["name"],
                offset=f["offset"],
                type=f.get("type", "?"),
                value=f.get("value"),
                hex_value=f.get("hex", ""),
                comment=f.get("comment", ""),
                points_to=f.get("points_to", ""),
                is_null=f.get("is_null", False)
            )

            # Smart follow: expand pointer fields if they're non-null and we have depth
            if (f.get("points_to") and
                not f.get("is_null", True) and
                depth > 0 and
                f.get("value", "0x00000000") != "0x00000000"):

                ptr_addr = f["value"]
                ptr_type = f["points_to"]

                # Check if we have a struct def for this type
                struct_def = self.game.call("get_struct_def", {"name": ptr_type})
                if "error" not in struct_def:
                    child = self.explore(ptr_addr, ptr_type, depth - 1, follow_nulls)
                    if child:
                        for cf in child.fields:
                            ef.children.append(cf)

            explored.fields.append(ef)

        return explored

    def discover(self, address: str, size: int = 256) -> Dict:
        """Analyze unknown memory region and propose struct layout.

        Classifies each field with heuristics:
        - Pointer: valid readable address, try to match known struct signatures
        - String: sequences of printable ASCII chars
        - Float: value in reasonable float range
        - Small int: < 0x10000 (likely enum, counter, index)
        - Flags: power of 2 patterns
        - Zero: null/unused
        - Large int: everything else
        """
        result = self.game.call("read_region", {
            "address": address,
            "size": size
        })

        if "error" in result:
            return {"error": result["error"]}

        # Also read raw bytes for string detection
        raw = self.game.call("read_memory", {
            "address": address,
            "size": size,
            "format": "all"
        })
        ascii_str = raw.get("ascii", "")

        proposed_fields = []
        known_structs = self.game.call("list_struct_defs").get("structs", [])

        for dw in result.get("dwords", []):
            offset = dw["offset"]
            val = dw["value"]
            hex_val = dw["hex"]
            classification = dw["type"]
            field_name = f"field_{offset:02X}"
            confidence = "unknown"
            pointed_struct = ""
            extra = {}

            if val == 0:
                classification = "zero"
                field_name = f"_pad_{offset:02X}"
                confidence = "likely"

            elif classification == "pointer":
                confidence = "likely"
                field_name = f"ptr_{offset:02X}"

                # Try to identify what it points to by reading first DWORD
                # and matching against known struct signatures
                peek = self.game.call("read_region", {
                    "address": hex_val,
                    "size": 32
                })
                if "error" not in peek:
                    preview = [d["hex"] for d in peek.get("dwords", [])[:4]]
                    extra["pointer_preview"] = preview

                    # Heuristic: try reading as each known struct and score
                    first_dword = peek.get("dwords", [{}])[0].get("value", 0)

                    # UnitAny signature: dwType 0-5 at offset 0
                    if first_dword <= 5:
                        pointed_struct = "UnitAny?"
                        confidence = "possible"

                    # Inventory signature: dwSignature == 0x01020304
                    if first_dword == 0x01020304:
                        pointed_struct = "Inventory"
                        confidence = "high"

            elif classification == "small_int":
                confidence = "likely"
                if val <= 5:
                    field_name = f"type_{offset:02X}"
                    extra["possible_enum"] = True
                elif val <= 100:
                    field_name = f"count_{offset:02X}"
                elif val <= 0xFFFF:
                    field_name = f"index_{offset:02X}"

            elif classification == "possible_string":
                # Check for actual string in raw bytes
                str_start = offset
                str_chars = ""
                for i in range(str_start, min(str_start + 32, len(ascii_str))):
                    if i < len(ascii_str) and ascii_str[i] != '.':
                        str_chars += ascii_str[i]
                    else:
                        break
                if len(str_chars) >= 3:
                    classification = "string"
                    field_name = f"sz_{offset:02X}"
                    extra["string_value"] = str_chars
                    confidence = "high"

            else:
                # Check if it looks like a float
                import struct as pystruct
                try:
                    float_val = pystruct.unpack('f', pystruct.pack('I', val & 0xFFFFFFFF))[0]
                    if 0.001 < abs(float_val) < 1e6 and not (val > 0x3F000000 and val < 0x47000000 and float_val == int(float_val)):
                        classification = "possible_float"
                        extra["float_value"] = round(float_val, 4)
                except:
                    pass

                # Check for flag patterns (powers of 2)
                if val != 0 and (val & (val - 1)) == 0:
                    extra["possible_flag"] = True

            proposed_fields.append({
                "offset": offset,
                "address": dw["address"],
                "value": val,
                "hex": hex_val,
                "classification": classification,
                "suggested_name": field_name,
                "confidence": confidence,
                "pointed_struct": pointed_struct,
                **extra
            })

        # Detect consecutive zero regions (padding)
        zero_runs = []
        run_start = None
        for f in proposed_fields:
            if f["classification"] == "zero":
                if run_start is None:
                    run_start = f["offset"]
            else:
                if run_start is not None:
                    run_end = f["offset"]
                    if run_end - run_start >= 8:
                        zero_runs.append({"start": run_start, "end": run_end, "size": run_end - run_start})
                    run_start = None

        return {
            "address": address,
            "size": size,
            "field_count": len(proposed_fields),
            "pointer_count": sum(1 for f in proposed_fields if f["classification"] == "pointer"),
            "string_count": sum(1 for f in proposed_fields if f["classification"] == "string"),
            "zero_count": sum(1 for f in proposed_fields if f["classification"] == "zero"),
            "padding_regions": zero_runs,
            "fields": proposed_fields,
            "timestamp": datetime.now().isoformat()
        }

    def compare(self, struct_name: str, address: str = None) -> Dict:
        """Compare our struct definition with Ghidra's and validate against live memory."""
        # Get our struct def
        our_def = self.game.call("get_struct_def", {"name": struct_name})
        if "error" in our_def:
            return {"error": f"Struct not found in registry: {struct_name}"}

        result = {
            "struct": struct_name,
            "our_size": our_def.get("size", 0),
            "our_field_count": len(our_def.get("fields", [])),
            "our_source": our_def.get("source", ""),
            "differences": [],
            "validations": []
        }

        # If we have an address, validate fields against live memory
        if address:
            live = self.game.call("read_struct", {
                "address": address,
                "struct_name": struct_name,
                "follow_pointers": False
            })
            if "error" not in live:
                for f in live.get("fields", []):
                    validation = {
                        "name": f["name"],
                        "offset": f["offset"],
                        "value": f.get("value"),
                        "valid": True
                    }

                    # Check pointer fields are actually valid pointers
                    if f.get("points_to") and f.get("value") and f["value"] != "0x00000000":
                        ptr_hex = f["value"]
                        peek = self.game.call("read_memory", {
                            "address": ptr_hex,
                            "size": 4,
                            "format": "hex"
                        })
                        if "error" in peek or "Cannot read" in str(peek):
                            validation["valid"] = False
                            validation["issue"] = f"Pointer {ptr_hex} is not readable"
                            result["differences"].append(validation)

                    result["validations"].append(validation)

        # Try to query Ghidra for its version
        try:
            ghidra = McpClient(self.ghidra_url)
            if ghidra.is_alive():
                # Search for struct in Ghidra
                ghidra_result = ghidra.call("search_data_types", {"pattern": struct_name})
                if "error" not in ghidra_result:
                    result["ghidra_types_found"] = ghidra_result
                result["ghidra_available"] = True
            else:
                result["ghidra_available"] = False
        except:
            result["ghidra_available"] = False

        return result


# ---- Output Formatters ----

def print_tree(struct: ExploredStruct, indent: int = 0):
    """Print struct as a tree."""
    prefix = "  " * indent
    print(f"{prefix}{struct.name} at {struct.address} ({struct.size} bytes)")

    for f in struct.fields:
        val_str = str(f.value) if f.value is not None else "?"
        comment = f"  // {f.comment}" if f.comment else ""
        ptr_info = f" -> {f.points_to}" if f.points_to else ""

        if f.is_null and f.points_to:
            print(f"{prefix}  +0x{f.offset:02X} {f.name:20s} = NULL{ptr_info}{comment}")
        elif f.hex_value:
            print(f"{prefix}  +0x{f.offset:02X} {f.name:20s} = {f.hex_value} ({val_str}){ptr_info}{comment}")
        else:
            print(f"{prefix}  +0x{f.offset:02X} {f.name:20s} = {val_str}{ptr_info}{comment}")

        # Print children
        if f.children:
            for child in f.children:
                child_val = str(child.value) if child.value is not None else "?"
                child_hex = f" ({child.hex_value})" if child.hex_value else ""
                child_comment = f"  // {child.comment}" if child.comment else ""
                child_ptr = f" -> {child.points_to}" if child.points_to else ""
                print(f"{prefix}    .{child.name:18s} = {child_val}{child_hex}{child_ptr}{child_comment}")
                # Show grandchildren
                for gc in child.children:
                    gc_val = str(gc.value) if gc.value is not None else "?"
                    print(f"{prefix}      .{gc.name:16s} = {gc_val}")


def export_json(struct: ExploredStruct, path: str):
    """Export explored struct as JSON."""
    data = {
        "name": struct.name,
        "address": struct.address,
        "size": struct.size,
        "source": struct.source,
        "timestamp": datetime.now().isoformat(),
        "fields": []
    }
    for f in struct.fields:
        fd = {
            "name": f.name,
            "offset": f.offset,
            "type": f.type,
            "value": f.value,
            "comment": f.comment
        }
        if f.points_to:
            fd["points_to"] = f.points_to
        if f.children:
            fd["children"] = [
                {"name": c.name, "offset": c.offset, "value": c.value}
                for c in f.children
            ]
        data["fields"].append(fd)

    with open(path, "w") as fout:
        json.dump(data, fout, indent=2, default=str)
    print(f"Exported to {path}")


def export_c_header(structs: Dict[str, Any], path: str):
    """Generate C header from struct definitions."""
    type_map = {
        "byte": "BYTE", "word": "WORD", "dword": "DWORD",
        "int": "int", "float": "float", "pointer": "void*",
        "string": "char", "wstring": "wchar_t", "padding": "BYTE"
    }

    with open(path, "w") as f:
        f.write("// Auto-generated struct definitions from d2-mod-toolkit\n")
        f.write(f"// Generated: {datetime.now().isoformat()}\n")
        f.write("#pragma once\n#include <windows.h>\n\n")

        for name, sdef in structs.items():
            f.write(f"struct {name} {{ // size 0x{sdef['size']:X}\n")
            for field in sdef.get("fields", []):
                ft = field.get("type", "padding")
                ct = type_map.get(ft, "DWORD")
                fname = field["name"]
                offset = field["offset"]
                size = field.get("size", 4)
                comment = field.get("comment", "")
                pts = field.get("points_to", "")

                if ft == "pointer" and pts:
                    f.write(f"    {pts}* {fname}; // 0x{offset:02X}")
                elif ft == "string":
                    f.write(f"    char {fname}[{size}]; // 0x{offset:02X}")
                elif ft == "padding":
                    f.write(f"    BYTE _{fname}[{size}]; // 0x{offset:02X}")
                elif ft == "array":
                    count = field.get("array_count", size // 4)
                    f.write(f"    DWORD {fname}[{count}]; // 0x{offset:02X}")
                else:
                    f.write(f"    {ct} {fname}; // 0x{offset:02X}")

                if comment:
                    f.write(f" {comment}")
                f.write("\n")

            f.write(f"}}; // 0x{sdef['size']:X}\n\n")

    print(f"Generated C header: {path}")


# ---- CLI ----

def main():
    parser = argparse.ArgumentParser(description="D2 Struct Explorer")
    parser.add_argument("--game-url", default="http://127.0.0.1:21337")
    parser.add_argument("--ghidra-url", default="http://127.0.0.1:8089")

    sub = parser.add_subparsers(dest="command")

    # explore
    p_explore = sub.add_parser("explore", help="Smart traversal from a known pointer")
    p_explore.add_argument("--address", required=True, help="Hex address")
    p_explore.add_argument("--type", required=True, help="Struct type name")
    p_explore.add_argument("--depth", type=int, default=2)
    p_explore.add_argument("--json", help="Export to JSON file")

    # discover
    p_discover = sub.add_parser("discover", help="Analyze unknown memory region")
    p_discover.add_argument("--address", required=True)
    p_discover.add_argument("--size", type=int, default=256)

    # compare
    p_compare = sub.add_parser("compare", help="Diff Ghidra vs runtime")
    p_compare.add_argument("--type", required=True)
    p_compare.add_argument("--address")

    # export
    p_export = sub.add_parser("export", help="Export struct definitions")
    p_export.add_argument("--output", default="structs/")
    p_export.add_argument("--format", choices=["json", "c", "both"], default="both")

    # propose
    p_propose = sub.add_parser("propose", help="Discover and propose a struct definition")
    p_propose.add_argument("--address", required=True)
    p_propose.add_argument("--size", type=int, default=128)
    p_propose.add_argument("--name", default="Unknown")
    p_propose.add_argument("--save", action="store_true", help="Save to struct registry")

    # list
    sub.add_parser("list", help="List known struct definitions")

    args = parser.parse_args()

    explorer = StructExplorer(args.game_url, args.ghidra_url)

    if not explorer.game.is_alive():
        print("ERROR: Game MCP server not responding at", args.game_url)
        sys.exit(1)

    if args.command == "explore":
        result = explorer.explore(args.address, args.type, args.depth)
        if result:
            print_tree(result)
            if args.json:
                export_json(result, args.json)

    elif args.command == "discover":
        result = explorer.discover(args.address, args.size)
        print(json.dumps(result, indent=2, default=str))

    elif args.command == "compare":
        result = explorer.compare(args.type, args.address)
        print(json.dumps(result, indent=2, default=str))

    elif args.command == "propose":
        result = explorer.discover(args.address, args.size)
        if "error" in result:
            print(f"Error: {result['error']}")
            sys.exit(1)

        print(f"\n=== Proposed struct: {args.name} ({args.size} bytes) ===")
        print(f"Pointers: {result['pointer_count']}  Strings: {result['string_count']}  Zeros: {result['zero_count']}")
        if result.get("padding_regions"):
            print(f"Padding regions: {result['padding_regions']}")
        print()

        type_map = {
            "pointer": "pointer", "string": "string", "zero": "padding",
            "small_int": "dword", "integer": "dword", "possible_float": "float",
            "possible_string": "dword", "unknown": "dword"
        }

        # Print proposed fields
        for f in result["fields"]:
            cls = f["classification"]
            name = f["suggested_name"]
            val = f["hex"]
            conf = f["confidence"]
            pts = f.get("pointed_struct", "")
            extra = ""
            if f.get("string_value"):
                extra = f' = "{f["string_value"]}"'
            if f.get("float_value"):
                extra = f" = {f['float_value']}"
            if pts:
                extra += f" -> {pts}"

            color = {"high": "", "likely": "", "possible": "?", "unknown": "??"}
            marker = color.get(conf, "")
            print(f"  +0x{f['offset']:02X}  {name:20s}  {cls:14s}  {val}  {marker}{extra}")

        # If --save, create a struct definition in the registry
        if args.save:
            print(f"\nSaving as '{args.name}' to struct registry...")
            # Build fields for the registry
            # Note: we can't call add_struct_def via MCP yet (tool doesn't exist)
            # For now, save to a JSON file
            struct_def = {
                args.name: {
                    "size": args.size,
                    "source": "discovered",
                    "fields": []
                }
            }
            for f in result["fields"]:
                cls = f["classification"]
                ft = type_map.get(cls, "dword")
                fd = {
                    "name": f["suggested_name"],
                    "offset": f["offset"],
                    "type": ft,
                    "size": 4
                }
                if f.get("pointed_struct"):
                    fd["points_to"] = f["pointed_struct"].rstrip("?")
                if f.get("string_value"):
                    fd["comment"] = f["string_value"]
                struct_def[args.name]["fields"].append(fd)

            outpath = f"discovered_{args.name}.json"
            with open(outpath, "w") as fout:
                json.dump(struct_def, fout, indent=2)
            print(f"Saved to {outpath}")

    elif args.command == "export":
        os.makedirs(args.output, exist_ok=True)
        structs = explorer.game.call("list_struct_defs")
        all_defs = {}
        for s in structs.get("structs", []):
            sdef = explorer.game.call("get_struct_def", {"name": s["name"]})
            if "error" not in sdef:
                all_defs[s["name"]] = sdef

        if args.format in ("json", "both"):
            path = os.path.join(args.output, "structs.json")
            with open(path, "w") as f:
                json.dump(all_defs, f, indent=2)
            print(f"Exported JSON: {path}")

        if args.format in ("c", "both"):
            path = os.path.join(args.output, "D2Structs_generated.h")
            export_c_header(all_defs, path)

    elif args.command == "list":
        structs = explorer.game.call("list_struct_defs")
        for s in structs.get("structs", []):
            print(f"  {s['name']:20s} size={s['size']:4d} fields={s['fields']:2d} src={s['source']}")

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
