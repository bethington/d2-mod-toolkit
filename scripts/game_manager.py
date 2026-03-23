"""Game Manager — Reliable lifecycle management for PD2.

Handles: kill, deploy, launch, navigate to in-game.
Single command to go from any state to fully in-game.

Usage:
    python game_manager.py                    # Full cycle: kill -> deploy -> launch -> navigate
    python game_manager.py --kill             # Kill all Game.exe processes
    python game_manager.py --launch           # Launch only (no kill/deploy)
    python game_manager.py --navigate         # Navigate to in-game (game must be running)
    python game_manager.py --status           # Check current state
    python game_manager.py --no-deploy        # Skip DLL deploy step
    python game_manager.py --character NAME   # Select specific character
"""

import subprocess
import sys
import time
import json
import os
import argparse

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
MCP_URL = "http://127.0.0.1:21337"
MCP_TIMEOUT = 5


def mcp_call(tool, args=None, timeout=MCP_TIMEOUT):
    """Call an MCP tool. Returns parsed result or None on failure."""
    try:
        r = requests.post(f"{MCP_URL}/mcp", json={
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


def mcp_alive():
    """Check if MCP server is responding."""
    return mcp_call("ping") is not None


def get_game_state():
    """Get current game state via MCP."""
    return mcp_call("get_game_state")


def kill_game():
    """Kill all Game.exe processes using multiple strategies."""
    print("Killing Game.exe...")

    # Strategy 1: MCP quit_game (graceful)
    if mcp_alive():
        print("  Trying graceful quit via MCP...")
        mcp_call("quit_game")
        time.sleep(5)
        if not _game_running():
            print("  Graceful quit succeeded")
            return True

    # Strategy 2: taskkill /F
    print("  Trying taskkill /F...")
    subprocess.run(["taskkill", "/F", "/IM", "Game.exe", "/T"],
                   capture_output=True, timeout=10)
    time.sleep(3)
    if not _game_running():
        print("  taskkill succeeded")
        return True

    # Strategy 3: PowerShell Stop-Process
    print("  Trying PowerShell Stop-Process...")
    subprocess.run(
        ["powershell", "-Command", "Stop-Process -Name Game -Force -ErrorAction SilentlyContinue"],
        capture_output=True, timeout=10
    )
    time.sleep(3)
    if not _game_running():
        print("  PowerShell kill succeeded")
        return True

    # Strategy 4: WMI terminate
    print("  Trying WMI terminate...")
    subprocess.run(
        ["powershell", "-Command",
         "Get-WmiObject Win32_Process -Filter \"Name='Game.exe'\" | ForEach-Object { $_.Terminate() }"],
        capture_output=True, timeout=10
    )
    time.sleep(3)
    if not _game_running():
        print("  WMI terminate succeeded")
        return True

    # Strategy 5: wmic
    print("  Trying wmic...")
    subprocess.run(
        ["wmic", "process", "where", "name='Game.exe'", "delete"],
        capture_output=True, timeout=10
    )
    time.sleep(3)
    if not _game_running():
        print("  wmic kill succeeded")
        return True

    # Strategy 6: Elevated taskkill via PowerShell Start-Process -Verb RunAs
    print("  Trying elevated taskkill (may show UAC prompt)...")
    try:
        subprocess.run(
            ["powershell", "-Command",
             "Start-Process taskkill -ArgumentList '/F /IM Game.exe /T' -Verb RunAs -Wait"],
            capture_output=True, timeout=30
        )
        time.sleep(3)
        if not _game_running():
            print("  Elevated taskkill succeeded")
            return True
    except:
        pass

    print("  ERROR: Could not kill Game.exe — try closing it manually")
    return False


def _game_running():
    """Check if Game.exe is in the process list."""
    result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Game.exe"],
                           capture_output=True, text=True, timeout=5)
    return "Game.exe" in result.stdout


def deploy_dll():
    """Deploy BH.dll to game folder."""
    if not os.path.exists(DLL_SOURCE):
        print(f"  ERROR: DLL not found at {DLL_SOURCE}")
        return False

    try:
        # Try direct copy
        import shutil
        shutil.copy2(DLL_SOURCE, DLL_DEST)
        print(f"  Deployed BH.dll ({os.path.getsize(DLL_DEST)} bytes)")
        return True
    except PermissionError:
        print("  ERROR: DLL is locked (game still running?)")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


def launch_game():
    """Launch Game.exe from the game directory."""
    if _game_running():
        print("  Game already running")
        return True

    print(f"  Launching {GAME_EXE} {' '.join(GAME_ARGS)}...")
    subprocess.Popen(
        [GAME_EXE] + GAME_ARGS,
        cwd=GAME_DIR,
        creationflags=subprocess.DETACHED_PROCESS
    )

    # Wait for MCP to come online
    print("  Waiting for MCP server...", end="", flush=True)
    for i in range(30):
        time.sleep(2)
        if mcp_alive():
            print(f" ready ({(i+1)*2}s)")
            return True
        print(".", end="", flush=True)

    print(" TIMEOUT")
    return False


def navigate_to_game(character=None):
    """Navigate from menu to in-game using click_control."""
    print("Navigating to game...")

    for attempt in range(15):
        state = get_game_state()
        if not state:
            print("  MCP not responding")
            return False

        if state.get("state") == "in_game":
            print(f"  Already in game! Area: {state.get('area_name', '?')}")
            return True

        controls = mcp_call("get_controls")
        if not controls:
            time.sleep(2)
            continue

        ctrl_list = controls.get("controls", [])
        screen = _identify_screen(ctrl_list)
        print(f"  Screen: {screen} ({len(ctrl_list)} controls)")

        if screen == "main_menu":
            _click_single_player(ctrl_list)
            time.sleep(3)

        elif screen == "gateway":
            # Cancel gateway, then retry Single Player
            _click_leftmost_button(ctrl_list)
            time.sleep(3)
            # Click SP again
            controls2 = mcp_call("get_controls")
            if controls2:
                _click_single_player(controls2.get("controls", []))
                time.sleep(3)

        elif screen == "char_select":
            if character:
                _click_character(ctrl_list, character)
                time.sleep(1)
            _click_ok_button(ctrl_list)
            time.sleep(3)

        elif screen == "difficulty":
            _click_highest_difficulty(ctrl_list)
            time.sleep(10)  # loading takes a while

        elif screen == "loading":
            time.sleep(5)

        else:
            print(f"  Unknown screen, waiting...")
            time.sleep(3)

    # Final check
    state = get_game_state()
    if state and state.get("state") == "in_game":
        print(f"  Success! In game: {state.get('area_name', '?')}")
        return True

    print("  Failed to navigate to game")
    return False


def _identify_screen(controls):
    """Identify the current screen from control patterns."""
    texts = []
    for c in controls:
        for t in c.get("text", []):
            texts.append(t.lower())

    buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")]
    textboxes = [c for c in controls if c["type"] == "textbox"]

    if not controls:
        return "loading"

    if len(controls) <= 3:
        return "loading"

    # Priority 1: Gateway (has "SELECT GATEWAY" text)
    if any("select gateway" in t for t in texts):
        return "gateway"

    # Priority 2: Char select (OK button at x>600 + many textboxes with names)
    has_ok = any(b["x"] > 600 and b["y"] > 550 for b in buttons)
    has_char_slots = len(textboxes) >= 8
    if has_ok and has_char_slots:
        # Check for difficulty overlay ON TOP of char select
        diff_buttons = [b for b in buttons if 250 <= b["x"] <= 280 and 280 <= b["y"] <= 400
                        and b["w"] >= 200]
        if len(diff_buttons) >= 2:
            return "difficulty"
        return "char_select"

    # Priority 3: Main menu (4+ buttons at x=264, NO char slots)
    menu_buttons = [b for b in buttons if b["x"] == 264]
    if len(menu_buttons) >= 4 and not has_char_slots:
        return "main_menu"

    # Priority 5: Any screen with 5+ buttons is likely main menu
    if len(buttons) >= 5:
        return "main_menu"

    return "unknown"


def _click_single_player(controls):
    """Click the Single Player button on the main menu."""
    for c in controls:
        if c["type"] == "button" and c.get("has_on_press") and c["x"] == 264 and c["y"] < 340:
            print(f"  Clicking Single Player (index {c['index']})")
            mcp_call("click_control", {"index": c["index"]})
            return


def _click_leftmost_button(controls):
    """Click the leftmost button (Cancel on gateway)."""
    buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")]
    if buttons:
        leftmost = min(buttons, key=lambda b: b["x"])
        print(f"  Clicking leftmost button (index {leftmost['index']})")
        mcp_call("click_control", {"index": leftmost["index"]})


def _click_ok_button(controls):
    """Click the OK button (bottom-right)."""
    for c in controls:
        if c["type"] == "button" and c.get("has_on_press") and c["x"] > 600 and c["y"] > 550:
            print(f"  Clicking OK (index {c['index']})")
            mcp_call("click_control", {"index": c["index"]})
            return


def _click_character(controls, name):
    """Click a character by name in the character select."""
    name_lower = name.lower()
    for c in controls:
        for t in c.get("text", []):
            if name_lower in t.lower():
                print(f"  Clicking character '{t}' (index {c['index']})")
                mcp_call("click_control", {"index": c["index"]})
                return
    print(f"  Character '{name}' not found")


def _click_highest_difficulty(controls):
    """Click the highest available difficulty button."""
    diff_buttons = [c for c in controls if c["type"] == "button" and c.get("has_on_press")
                    and 250 <= c["x"] <= 280 and 280 <= c["y"] <= 400]
    if diff_buttons:
        highest = max(diff_buttons, key=lambda b: b["y"])
        print(f"  Clicking highest difficulty (index {highest['index']})")
        mcp_call("click_control", {"index": highest["index"]})


def full_cycle(character=None, skip_deploy=False):
    """Full lifecycle: kill -> deploy -> launch -> navigate."""
    print("=== Game Manager: Full Cycle ===\n")

    # Kill
    if _game_running():
        if not kill_game():
            print("\nCannot kill Game.exe — please close it manually")
            return False
    else:
        print("No Game.exe running")

    # Deploy
    if not skip_deploy:
        print("\nDeploying BH.dll...")
        if not deploy_dll():
            return False
    else:
        print("\nSkipping DLL deploy")

    # Launch
    print("\nLaunching game...")
    if not launch_game():
        return False

    # Navigate
    print()
    if not navigate_to_game(character):
        return False

    print("\n=== Ready! Game is running and in-game ===")
    return True


def main():
    parser = argparse.ArgumentParser(description="PD2 Game Manager")
    parser.add_argument("--kill", action="store_true", help="Kill Game.exe")
    parser.add_argument("--launch", action="store_true", help="Launch game only")
    parser.add_argument("--navigate", action="store_true", help="Navigate to in-game")
    parser.add_argument("--status", action="store_true", help="Show current state")
    parser.add_argument("--no-deploy", action="store_true", help="Skip DLL deploy")
    parser.add_argument("--character", type=str, help="Character name to select")
    args = parser.parse_args()

    if args.status:
        running = _game_running()
        print(f"Game process: {'running' if running else 'not running'}")
        if running and mcp_alive():
            state = get_game_state()
            print(f"MCP: alive")
            print(f"State: {state}")
        elif running:
            print("MCP: not responding")
        return

    if args.kill:
        kill_game()
        return

    if args.launch:
        launch_game()
        return

    if args.navigate:
        navigate_to_game(args.character)
        return

    # Default: full cycle
    full_cycle(character=args.character, skip_deploy=args.no_deploy)


if __name__ == "__main__":
    main()
