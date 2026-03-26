---
name: build-and-deploy
description: Compile C++ BH.dll, deploy to game directory, restart Diablo 2, and verify the game is running
---

# Build & Deploy

Compile the d2-mod-toolkit BH.dll and hot-deploy it into the running Diablo 2 game.

## When to use

Use this skill whenever you have made changes to C++ source files in the `BH/` directory and need to compile, deploy, and test them.

## How to use

Run the build script from the project root:

```bash
python scripts/build_and_deploy.py --json
```

### Modes

- **Full cycle** (default): Compile → kill game → deploy DLL → launch game → navigate to in-game → verify
- **Compile only**: `python scripts/build_and_deploy.py --compile-only --json` — just check if code compiles
- **Deploy only**: `python scripts/build_and_deploy.py --deploy-only --json` — skip compile, redeploy existing DLL
- **Specific character**: `python scripts/build_and_deploy.py --character combustion --json`

### Output

The script returns JSON with structured results:

- `success`: true/false
- `phase`: which phase completed or failed (compile, kill, deploy, launch, navigate, verify)
- `errors`: array of compile errors with file, line, col, code, message
- `dll_size`: size of compiled DLL in bytes
- `game_area`: which game area the character is in after deploy

### On failure

- **Compile errors**: Read the `errors` array. Fix the C++ code and retry.
- **DLL locked**: The game is holding the DLL. Kill the game first with `python scripts/game_manager.py --kill`, then retry.
- **Stale cl.exe**: The script auto-kills stale compiler processes, but if it persists, run: `powershell -Command "Stop-Process -Name cl -Force"`
- **Game won't launch**: Check that `C:\Diablo2\ProjectD2_dlls_removed\Game.exe` exists.
- **Navigate fails**: Use `python scripts/game_manager.py --navigate` separately to debug menu navigation.

### Important

- Game lifecycle (kill, deploy, launch, navigate) is managed by `scripts/game_manager.py`
- The DLL builds to `Release/BH.dll` in the project root
- MSBuild path: `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`
- Always use `--json` flag when calling programmatically
