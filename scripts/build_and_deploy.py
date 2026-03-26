"""Build & Deploy Skill — Atomic compile, deploy, restart, verify.

Designed to be called by an autonomous agent (OpenClaw/Claude).
Returns structured JSON so the agent can parse results programmatically.

Game lifecycle (kill, deploy, launch, navigate) is delegated to game_manager.py.

Usage:
    python build_and_deploy.py                  # Full: compile + deploy + restart + verify
    python build_and_deploy.py --compile-only   # Just compile, report errors
    python build_and_deploy.py --deploy-only    # Deploy + restart (skip compile)
    python build_and_deploy.py --character NAME  # Select specific character after restart
    python build_and_deploy.py --json           # Output JSON instead of human-readable
"""

import subprocess
import sys
import os
import re
import json

# Paths
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
SLN_PATH = os.path.join(PROJECT_DIR, "BH.sln")
DLL_OUTPUT = os.path.join(PROJECT_DIR, "Release", "BH.dll")
MSBUILD = r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
MCP_URL = "http://127.0.0.1:21337"

# Import game_manager for all game lifecycle operations
sys.path.insert(0, SCRIPTS_DIR)
import game_manager

try:
    import requests
except ImportError:
    requests = None


def mcp_call(tool, args=None, timeout=10):
    if not requests:
        return None
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
    return mcp_call("ping") is not None


def kill_stale_compilers():
    """Kill any stale cl.exe processes that could lock PDB files."""
    try:
        subprocess.run(
            ["powershell", "-Command",
             "Stop-Process -Name cl -Force -ErrorAction SilentlyContinue"],
            capture_output=True, timeout=5
        )
    except:
        pass


def unlock_dll():
    """Try to unlock BH.dll if it's held by a dead process."""
    if not os.path.exists(DLL_OUTPUT):
        return True
    tmp = DLL_OUTPUT + ".old"
    try:
        if os.path.exists(tmp):
            os.remove(tmp)
        os.rename(DLL_OUTPUT, tmp)
        os.remove(tmp)
        return True
    except:
        pass
    try:
        os.remove(DLL_OUTPUT)
        return True
    except:
        return False


def compile_project():
    """Compile BH.sln. Returns (success, errors, warnings, raw_output)."""
    if not os.path.exists(MSBUILD):
        return False, [{"message": f"MSBuild not found at {MSBUILD}"}], [], ""

    cmd = [
        MSBUILD, SLN_PATH,
        "-p:Configuration=Release",
        "-p:Platform=Win32",
        "-nologo",
        "-verbosity:minimal",
        "-consoleloggerparameters:NoSummary"
    ]

    # Pre-flight: kill stale compilers and try to unlock DLL
    kill_stale_compilers()
    unlock_dll()

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, cwd=PROJECT_DIR)
    except subprocess.TimeoutExpired:
        return False, [{"message": "Build timed out after 5 minutes"}], [], ""

    output = result.stdout + result.stderr
    errors = []
    warnings = []

    for line in output.splitlines():
        # Match MSBuild error/warning format: file(line,col): error/warning CODE: message
        m = re.match(r'^(.+?)\((\d+),(\d+)\):\s+(error|warning)\s+(\w+):\s+(.+?)(?:\s+\[.+\])?$', line)
        if m:
            entry = {
                "file": m.group(1),
                "line": int(m.group(2)),
                "col": int(m.group(3)),
                "code": m.group(5),
                "message": m.group(6).strip()
            }
            try:
                entry["file"] = os.path.relpath(entry["file"], PROJECT_DIR)
            except ValueError:
                pass

            if m.group(4) == "error":
                errors.append(entry)
            else:
                warnings.append(entry)
        elif "fatal error" in line.lower() or ": error " in line.lower():
            errors.append({"message": line.strip()})

    success = result.returncode == 0 and len(errors) == 0
    return success, errors, warnings, output


def verify_game():
    """Verify game is running and MCP is responsive."""
    if not mcp_alive():
        return False, "MCP not responding"

    state = mcp_call("get_game_state")
    if not state:
        return False, "Could not get game state"

    if state.get("state") != "in_game":
        return False, f"Game state is '{state.get('state')}', expected 'in_game'"

    player = mcp_call("get_player_state")
    area = player.get("area_name", "Unknown") if player else "Unknown"
    return True, f"Verified: in-game, area={area}"


def full_build_and_deploy(character=None, compile_only=False, deploy_only=False):
    """Full atomic operation. Returns structured result dict."""
    result = {
        "success": False,
        "phase": None,
        "steps": {},
        "errors": [],
        "warnings_count": 0,
        "dll_size": None,
        "game_area": None,
    }

    # Phase 1: Compile
    if not deploy_only:
        result["phase"] = "compile"
        compiled, errors, warnings, raw = compile_project()
        result["steps"]["compile"] = {
            "success": compiled,
            "errors": errors,
            "warnings_count": len(warnings),
        }
        result["warnings_count"] = len(warnings)

        if not compiled:
            result["errors"] = errors
            return result

        if os.path.exists(DLL_OUTPUT):
            result["dll_size"] = os.path.getsize(DLL_OUTPUT)

        if compile_only:
            result["success"] = True
            result["phase"] = "compile_only"
            return result

    # Phase 2: Kill game (via game_manager)
    result["phase"] = "kill"
    if game_manager._game_running():
        killed = game_manager.kill_game()
        result["steps"]["kill"] = {"success": killed}
        if not killed:
            result["errors"] = [{"message": "Could not kill Game.exe"}]
            return result
    else:
        result["steps"]["kill"] = {"success": True, "note": "not running"}

    # Phase 3: Deploy DLL (via game_manager)
    result["phase"] = "deploy"
    deployed = game_manager.deploy_dll()
    result["steps"]["deploy"] = {"success": deployed}
    if not deployed:
        result["errors"] = [{"message": "Failed to deploy DLL"}]
        return result

    # Phase 4: Launch game (via game_manager)
    result["phase"] = "launch"
    launched = game_manager.launch_game()
    result["steps"]["launch"] = {"success": launched}
    if not launched:
        result["errors"] = [{"message": "Game failed to launch or MCP did not come online"}]
        return result

    # Phase 5: Navigate to in-game (via game_manager)
    result["phase"] = "navigate"
    navigated = game_manager.navigate_to_game(character)
    result["steps"]["navigate"] = {"success": navigated}
    if not navigated:
        result["errors"] = [{"message": "Failed to navigate to in-game"}]
        return result

    # Phase 6: Verify
    result["phase"] = "verify"
    verified, verify_msg = verify_game()
    result["steps"]["verify"] = {"success": verified, "message": verify_msg}
    if not verified:
        result["errors"] = [{"message": verify_msg}]
        return result

    if "area=" in verify_msg:
        result["game_area"] = verify_msg.split("area=")[1]

    result["success"] = True
    result["phase"] = "complete"
    return result


def print_human(result):
    """Pretty-print result for human consumption."""
    phases = ["compile", "kill", "deploy", "launch", "navigate", "verify"]
    for phase in phases:
        step = result["steps"].get(phase)
        if not step:
            continue
        status = "OK" if step["success"] else "FAIL"
        extra = ""
        if "message" in step:
            extra = f" — {step['message']}"
        if "warnings_count" in step and step["warnings_count"] > 0:
            extra += f" ({step['warnings_count']} warnings)"
        print(f"  [{status}] {phase}{extra}")

    if result["errors"]:
        print(f"\nErrors:")
        for e in result["errors"]:
            if "file" in e:
                print(f"  {e['file']}({e['line']},{e['col']}): {e['code']}: {e['message']}")
            else:
                print(f"  {e['message']}")

    if result["success"]:
        print(f"\nBuild & deploy succeeded.")
        if result.get("game_area"):
            print(f"Game is running in: {result['game_area']}")
        if result.get("dll_size"):
            print(f"DLL size: {result['dll_size']:,} bytes")
    else:
        print(f"\nFailed at phase: {result['phase']}")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Build & Deploy Skill")
    parser.add_argument("--compile-only", action="store_true", help="Only compile, don't deploy")
    parser.add_argument("--deploy-only", action="store_true", help="Skip compile, just deploy + restart")
    parser.add_argument("--character", type=str, help="Character name to select")
    parser.add_argument("--json", action="store_true", help="Output JSON")
    args = parser.parse_args()

    print("=== Build & Deploy Skill ===\n")

    result = full_build_and_deploy(
        character=args.character,
        compile_only=args.compile_only,
        deploy_only=args.deploy_only,
    )

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print_human(result)

    sys.exit(0 if result["success"] else 1)


if __name__ == "__main__":
    main()
