"""PD2 Orchestrator — Persistent MCP server for game lifecycle management.

Runs independently of the game process. Survives game crashes, restarts,
and character switches. Proxies calls to the in-game MCP (BH.dll) when
the game is running, and handles lifecycle operations directly.

Usage:
    python orchestrator.py                # Start on default port 21338
    python orchestrator.py --port 21338   # Start on specific port

MCP Tools:
    get_status          — Game running? MCP alive? What character? What state?
    switch_character    — Exit current game, relaunch with different character
    new_game            — Exit current game, relaunch with same character
    exit_to_menu        — Exit current game (process dies, needs relaunch)
    launch_game         — Deploy DLL and launch game with character
    kill_game           — Force kill the game process
    restart_if_crashed  — Detect dead game, auto-relaunch
    list_characters     — List all characters (reads from in-game MCP)
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)

# Configuration
GAME_DIR = r"C:\Diablo2\ProjectD2_dlls_removed"
GAME_EXE = os.path.join(GAME_DIR, "Game.exe")
GAME_ARGS = ["-3dfx"]
DLL_SOURCE = r"C:\Users\benam\source\cpp\d2-mod-toolkit\BH\Release\BH.dll"
DLL_DEST = os.path.join(GAME_DIR, "BH.dll")
INGAME_MCP = "http://127.0.0.1:21337"
ORCH_PORT = 21338

# State
g_current_character = None
g_current_difficulty = 2  # 0=Normal, 1=NM, 2=Hell


def ingame_mcp(tool, args=None, timeout=5):
    """Call the in-game MCP server (BH.dll). Returns parsed result or None."""
    try:
        r = requests.post(f"{INGAME_MCP}/mcp", json={
            "jsonrpc": "2.0", "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args or {}}
        }, timeout=timeout)
        text = r.json().get("result", {}).get("content", [{}])[0].get("text", "")
        if text.startswith("{"):
            return json.loads(text)
        return {"_raw": text}
    except:
        return None


def ingame_alive():
    """Check if in-game MCP is responding."""
    return ingame_mcp("ping") is not None


def game_running():
    """Check if Game.exe process exists."""
    try:
        result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Game.exe"],
                                capture_output=True, text=True, timeout=5)
        return "Game.exe" in result.stdout
    except:
        return False


def kill_game():
    """Kill the game process. Tries clean exit first, then force kill."""
    if not game_running():
        return {"status": "not_running"}

    # Strategy 1: Clean exit via D2CLIENT_ExitGame → main menu → click "EXIT DIABLO II"
    if ingame_alive():
        ingame_mcp("exit_game")
        # Wait for game to reach main menu
        for i in range(20):
            time.sleep(0.5)
            if not game_running():
                return {"status": "clean_exit"}
            # Check if we're at the main menu now
            state = ingame_mcp("get_game_state")
            if state and state.get("state") == "menu":
                # At main menu — find and click "EXIT DIABLO II" button
                time.sleep(1)
                controls = ingame_mcp("get_controls")
                if controls:
                    for c in controls.get("controls", []):
                        # Exit button is typically the bottom button on main menu
                        # It's a button with OnPress at y > 550
                        if (c["type"] == "button" and c.get("has_on_press")
                                and c.get("y", 0) > 550 and c.get("x", 0) > 200):
                            ingame_mcp("click_control", {"index": c["index"]})
                            time.sleep(3)
                            if not game_running():
                                return {"status": "clean_exit"}
                            break
                break
        # Wait a bit more for process to die after clicking exit
        for i in range(10):
            time.sleep(1)
            if not game_running():
                return {"status": "clean_exit"}

    # Strategy 2: taskkill (no elevation)
    subprocess.run(["taskkill", "/F", "/IM", "Game.exe", "/T"],
                    capture_output=True, timeout=10)
    time.sleep(3)
    if not game_running():
        return {"status": "force_killed"}

    # Strategy 3: PowerShell Stop-Process
    subprocess.run(
        ["powershell", "-Command", "Stop-Process -Name Game -Force -ErrorAction SilentlyContinue"],
        capture_output=True, timeout=10)
    time.sleep(3)
    if not game_running():
        return {"status": "powershell_killed"}

    # Strategy 4: WMI terminate
    subprocess.run(
        ["powershell", "-Command",
         "Get-WmiObject Win32_Process -Filter \"Name='Game.exe'\" | ForEach-Object { $_.Terminate() }"],
        capture_output=True, timeout=10)
    time.sleep(3)
    if not game_running():
        return {"status": "wmi_killed"}

    return {"status": "failed", "error": "Could not kill Game.exe — close it manually"}


def deploy_dll():
    """Deploy BH.dll to game folder."""
    if not os.path.exists(DLL_SOURCE):
        return False, f"DLL not found at {DLL_SOURCE}"
    try:
        shutil.copy2(DLL_SOURCE, DLL_DEST)
        return True, f"Deployed ({os.path.getsize(DLL_DEST)} bytes)"
    except Exception as e:
        return False, str(e)


def launch_game():
    """Launch game and wait for MCP."""
    if game_running():
        if ingame_alive():
            return True, "Already running"
        return True, "Running but MCP not ready"

    subprocess.Popen([GAME_EXE] + GAME_ARGS, cwd=GAME_DIR,
                     creationflags=subprocess.DETACHED_PROCESS)

    for i in range(30):
        time.sleep(2)
        if ingame_alive():
            return True, f"Ready ({(i+1)*2}s)"

    return False, "MCP server timeout"


def navigate_to_game(character=None, difficulty=2):
    """Navigate from menus to in-game."""
    global g_current_character

    for attempt in range(15):
        state = ingame_mcp("get_game_state")
        if not state:
            time.sleep(2)
            continue

        if state.get("state") == "in_game":
            ps = ingame_mcp("get_player_state")
            name = ps.get("name", "?") if ps else "?"
            g_current_character = name
            return True, f"In game as {name}"

        controls = ingame_mcp("get_controls")
        if not controls:
            time.sleep(2)
            continue

        ctrl_list = controls.get("controls", [])
        screen = identify_screen(ctrl_list)

        if screen == "main_menu":
            click_single_player(ctrl_list)
            time.sleep(3)
        elif screen == "char_select":
            if character:
                select_character_direct(character)
            click_ok_button(ctrl_list)
            time.sleep(3)
        elif screen == "difficulty":
            click_highest_difficulty(ctrl_list)
            time.sleep(8)
        elif screen == "loading":
            time.sleep(5)
        elif screen == "gateway":
            click_leftmost_button(ctrl_list)
            time.sleep(3)
        else:
            time.sleep(3)

    return False, "Navigation timeout"


def select_character_direct(name):
    """Select character by writing g_nSelectedCharIndex directly."""
    # Walk the character linked list to find the index
    head_mem = ingame_mcp("read_memory", {"address": "0x6FA65EC8", "size": 4})
    if not head_mem:
        return False

    head = struct.unpack('<I', bytes.fromhex(head_mem['hex'].replace(' ', '')))[0]
    ptr = head
    for i in range(150):
        if ptr == 0:
            break
        name_mem = ingame_mcp("read_memory", {"address": hex(ptr), "size": 64})
        if not name_mem:
            break
        raw = bytes.fromhex(name_mem['hex'].replace(' ', ''))
        char_name = raw.split(b'\x00')[0].decode('ascii', errors='replace')
        if char_name.lower() == name.lower():
            # Write the selected index
            idx_bytes = struct.pack('<I', i)
            ingame_mcp("write_memory", {"address": "0x6FA64DB0", "bytes": idx_bytes.hex()})
            time.sleep(0.3)
            return True
        next_mem = ingame_mcp("read_memory", {"address": hex(ptr + 0x34C), "size": 4})
        if not next_mem:
            break
        ptr = struct.unpack('<I', bytes.fromhex(next_mem['hex'].replace(' ', '')))[0]

    return False


def list_characters():
    """List all characters by walking the linked list."""
    if not ingame_alive():
        return {"error": "In-game MCP not available"}

    head_mem = ingame_mcp("read_memory", {"address": "0x6FA65EC8", "size": 4})
    if not head_mem:
        return {"error": "Could not read character list"}

    head = struct.unpack('<I', bytes.fromhex(head_mem['hex'].replace(' ', '')))[0]
    count_mem = ingame_mcp("read_memory", {"address": "0x6FA65ED0", "size": 4})
    total = struct.unpack('<I', bytes.fromhex(count_mem['hex'].replace(' ', '')))[0] if count_mem else 0

    chars = []
    ptr = head
    for i in range(min(total + 5, 200)):
        if ptr == 0:
            break
        name_mem = ingame_mcp("read_memory", {"address": hex(ptr), "size": 64})
        if not name_mem:
            break
        raw = bytes.fromhex(name_mem['hex'].replace(' ', ''))
        name = raw.split(b'\x00')[0].decode('ascii', errors='replace')
        chars.append({"index": i, "name": name})
        next_mem = ingame_mcp("read_memory", {"address": hex(ptr + 0x34C), "size": 4})
        if not next_mem:
            break
        ptr = struct.unpack('<I', bytes.fromhex(next_mem['hex'].replace(' ', '')))[0]

    return {"count": len(chars), "characters": chars}


def identify_screen(controls):
    """Identify current screen from controls."""
    texts = []
    for c in controls:
        for t in c.get("text", []):
            texts.append(t.lower())

    buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")]
    textboxes = [c for c in controls if c["type"] == "textbox"]

    if not controls or len(controls) <= 3:
        return "loading"
    if any("select hero class" in t for t in texts):
        return "create_character"
    if any("select gateway" in t for t in texts):
        return "gateway"

    has_ok = any(b["x"] > 600 and b["y"] > 550 for b in buttons)
    has_char_slots = len(textboxes) >= 8
    if has_ok and has_char_slots:
        diff_buttons = [b for b in buttons if 250 <= b["x"] <= 280 and 280 <= b["y"] <= 400 and b["w"] >= 200]
        if len(diff_buttons) >= 2:
            return "difficulty"
        return "char_select"

    menu_buttons = [b for b in buttons if b["x"] == 264]
    if len(menu_buttons) >= 4 and not has_char_slots:
        return "main_menu"
    if len(buttons) >= 5:
        return "main_menu"

    return "unknown"


def click_single_player(controls):
    for c in controls:
        if c["type"] == "button" and c.get("has_on_press") and c["x"] == 264 and c["y"] < 340:
            ingame_mcp("click_control", {"index": c["index"]})
            return


def click_ok_button(controls):
    for c in controls:
        if c["type"] == "button" and c.get("has_on_press") and c["x"] > 600 and c["y"] > 550:
            ingame_mcp("click_control", {"index": c["index"]})
            return


def click_highest_difficulty(controls):
    diff_buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")
                    and 250 <= c["x"] <= 280 and 280 <= c["y"] <= 400]
    if diff_buttons:
        highest = max(diff_buttons, key=lambda b: b["y"])
        ingame_mcp("click_control", {"index": highest["index"]})


def click_leftmost_button(controls):
    buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")]
    if buttons:
        leftmost = min(buttons, key=lambda b: b["x"])
        ingame_mcp("click_control", {"index": leftmost["index"]})


# ─── MCP Tool Handlers ───────────────────────────────────────────────

def handle_get_status(args):
    running = game_running()
    alive = ingame_alive() if running else False
    state = ingame_mcp("get_game_state") if alive else None
    player = ingame_mcp("get_player_state") if alive and state and state.get("state") == "in_game" else None

    return {
        "game_running": running,
        "mcp_alive": alive,
        "state": state.get("state") if state else "not_running",
        "character": player.get("name") if player else g_current_character,
        "area": state.get("area_name") if state else None,
        "difficulty": state.get("difficulty") if state else None,
    }


def handle_switch_character(args):
    global g_current_character
    character = args.get("character")
    difficulty = args.get("difficulty", 2)
    skip_deploy = args.get("skip_deploy", False)

    if not character:
        return {"error": "character name required"}

    # Step 1: Exit current game cleanly (no force kill)
    kill_result = {"status": "not_running"}
    if game_running():
        if ingame_alive():
            ingame_mcp("exit_game")
        # Wait for process to die naturally (game exits after reaching main menu)
        for i in range(30):
            time.sleep(1)
            if not game_running():
                kill_result = {"status": "clean_exit"}
                break
        else:
            # Process didn't die — force kill as last resort
            kill_result = kill_game()

    # Wait for file handles to release
    time.sleep(2)

    # Deploy DLL if source is newer than dest
    if not skip_deploy:
        try:
            src_time = os.path.getmtime(DLL_SOURCE)
            dst_time = os.path.getmtime(DLL_DEST) if os.path.exists(DLL_DEST) else 0
            if src_time > dst_time:
                ok, msg = deploy_dll()
                if not ok:
                    time.sleep(2)
                    deploy_dll()  # retry once
        except:
            pass

    # Launch
    ok, msg = launch_game()
    if not ok:
        return {"error": f"Launch failed: {msg}"}

    # Navigate
    ok, msg = navigate_to_game(character, difficulty)
    g_current_character = character if ok else None

    return {
        "status": "success" if ok else "failed",
        "character": character,
        "message": msg,
        "kill_result": kill_result.get("status") if isinstance(kill_result, dict) else str(kill_result)
    }


def handle_new_game(args):
    character = args.get("character", g_current_character)
    difficulty = args.get("difficulty", 2)

    if not character:
        return {"error": "No character specified and no previous character known"}

    return handle_switch_character({"character": character, "difficulty": difficulty})


def handle_exit_to_menu(args):
    if not ingame_alive():
        return {"status": "not_in_game"}

    ingame_mcp("exit_game")
    time.sleep(3)

    return {
        "status": "exited",
        "game_running": game_running(),
        "mcp_alive": ingame_alive()
    }


def handle_launch_game(args):
    character = args.get("character", g_current_character)
    difficulty = args.get("difficulty", 2)
    deploy = args.get("deploy", True)

    if deploy:
        if game_running():
            kill_game()
            time.sleep(2)
        ok, msg = deploy_dll()
        if not ok:
            return {"error": f"Deploy failed: {msg}"}

    ok, msg = launch_game()
    if not ok:
        return {"error": f"Launch failed: {msg}"}

    if character:
        ok, msg = navigate_to_game(character, difficulty)
        return {"status": "in_game" if ok else "at_menu", "character": character, "message": msg}

    return {"status": "launched", "message": msg}


def handle_kill_game(args):
    return kill_game()


def handle_restart_if_crashed(args):
    if game_running() and ingame_alive():
        return {"status": "running", "message": "Game is running fine"}

    character = args.get("character", g_current_character)
    if not character:
        return {"error": "No character to restart with"}

    return handle_switch_character({"character": character})


def handle_list_characters(args):
    return list_characters()


def handle_proxy(args):
    """Proxy a call to the in-game MCP."""
    tool = args.get("tool")
    tool_args = args.get("args", {})
    timeout = args.get("timeout", 10)

    if not tool:
        return {"error": "tool name required"}

    result = ingame_mcp(tool, tool_args, timeout=timeout)
    if result is None:
        return {"error": "In-game MCP not responding"}
    return result


# ─── Tool Registry ───────────────────────────────────────────────────

TOOLS = {
    "get_status": {
        "description": "Get game status: running, MCP alive, character, area, state.",
        "handler": handle_get_status,
        "schema": {"type": "object", "properties": {}, "required": []}
    },
    "switch_character": {
        "description": "Exit current game, relaunch with a different character. Handles full cycle: kill → deploy → launch → navigate.",
        "handler": handle_switch_character,
        "schema": {"type": "object", "properties": {
            "character": {"type": "string", "description": "Character name to select"},
            "difficulty": {"type": "integer", "description": "0=Normal, 1=NM, 2=Hell (default 2)"}
        }, "required": ["character"]}
    },
    "new_game": {
        "description": "Create a new game with the same or specified character. Exits current game and relaunches.",
        "handler": handle_new_game,
        "schema": {"type": "object", "properties": {
            "character": {"type": "string", "description": "Character name (default: current)"},
            "difficulty": {"type": "integer", "description": "0=Normal, 1=NM, 2=Hell (default 2)"}
        }, "required": []}
    },
    "exit_to_menu": {
        "description": "Exit current game to main menu. Game process may terminate.",
        "handler": handle_exit_to_menu,
        "schema": {"type": "object", "properties": {}, "required": []}
    },
    "launch_game": {
        "description": "Deploy DLL, launch game, optionally navigate to a character.",
        "handler": handle_launch_game,
        "schema": {"type": "object", "properties": {
            "character": {"type": "string", "description": "Character to select and enter game with"},
            "difficulty": {"type": "integer", "description": "0=Normal, 1=NM, 2=Hell (default 2)"},
            "deploy": {"type": "boolean", "description": "Deploy DLL before launching (default true)"}
        }, "required": []}
    },
    "kill_game": {
        "description": "Force kill the game process.",
        "handler": handle_kill_game,
        "schema": {"type": "object", "properties": {}, "required": []}
    },
    "restart_if_crashed": {
        "description": "Check if game crashed and relaunch if needed.",
        "handler": handle_restart_if_crashed,
        "schema": {"type": "object", "properties": {
            "character": {"type": "string", "description": "Character to restart with (default: last used)"}
        }, "required": []}
    },
    "list_characters": {
        "description": "List all characters on the account. Game must be running at char select or in-game.",
        "handler": handle_list_characters,
        "schema": {"type": "object", "properties": {}, "required": []}
    },
    "proxy": {
        "description": "Proxy a tool call to the in-game MCP (BH.dll). Use for any game-specific tool.",
        "handler": handle_proxy,
        "schema": {"type": "object", "properties": {
            "tool": {"type": "string", "description": "In-game MCP tool name"},
            "args": {"type": "object", "description": "Tool arguments"},
            "timeout": {"type": "integer", "description": "Timeout in seconds (default 10)"}
        }, "required": ["tool"]}
    },
}


# ─── HTTP Server ─────────────────────────────────────────────────────

class McpHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress logs

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_POST(self):
        if self.path != "/mcp":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)

        try:
            req = json.loads(body)
        except:
            self.send_json_response({"error": "Invalid JSON"}, req_id=None)
            return

        req_id = req.get("id")
        method = req.get("method", "")
        params = req.get("params", {})

        if method == "initialize":
            self.send_json_response({
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "pd2-orchestrator", "version": "1.0"},
                "capabilities": {"tools": {"listChanged": False}}
            }, req_id)
            return

        if method == "tools/list":
            tools_list = []
            for name, info in TOOLS.items():
                tools_list.append({
                    "name": name,
                    "description": info["description"],
                    "inputSchema": info["schema"]
                })
            self.send_json_response({"tools": tools_list}, req_id)
            return

        if method == "tools/call":
            tool_name = params.get("name", "")
            tool_args = params.get("arguments", {})

            if tool_name not in TOOLS:
                self.send_json_response({
                    "content": [{"type": "text", "text": f"Unknown tool: {tool_name}"}],
                    "isError": True
                }, req_id)
                return

            try:
                result = TOOLS[tool_name]["handler"](tool_args)
                text = json.dumps(result, indent=2) if isinstance(result, dict) else str(result)
                self.send_json_response({
                    "content": [{"type": "text", "text": text}]
                }, req_id)
            except Exception as e:
                self.send_json_response({
                    "content": [{"type": "text", "text": f"Error: {e}"}],
                    "isError": True
                }, req_id)
            return

        self.send_json_response({"error": f"Unknown method: {method}"}, req_id)

    def send_json_response(self, result, req_id):
        response = {"jsonrpc": "2.0", "id": req_id, "result": result}
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Connection", "close")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description="PD2 Orchestrator MCP Server")
    parser.add_argument("--port", type=int, default=ORCH_PORT, help=f"Port (default {ORCH_PORT})")
    args = parser.parse_args()

    server = HTTPServer(("127.0.0.1", args.port), McpHandler)
    print(f"PD2 Orchestrator running on http://127.0.0.1:{args.port}/mcp")
    print(f"In-game MCP: {INGAME_MCP}")
    print(f"Tools: {', '.join(TOOLS.keys())}")
    print()

    # Status check
    if game_running():
        if ingame_alive():
            state = ingame_mcp("get_game_state")
            print(f"Game: running, MCP alive, state={state.get('state', '?') if state else '?'}")
        else:
            print("Game: running, MCP not responding")
    else:
        print("Game: not running")

    print("\nReady.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
