# NDI + OBS Setup: VM → Host Streaming

## Overview

The Windows VM runs the game + OpenClaw. NDI captures the VM's screen and sends it over the local network to your host machine, where OBS composites it with overlays and streams to Twitch.

```
VM (Windows)                    Host Machine
┌──────────────┐               ┌──────────────────┐
│ Diablo 2     │               │ OBS Studio       │
│ OpenClaw     │──── NDI ────→ │ ├─ NDI Source (VM)│
│ NDI Screen   │   (network)   │ ├─ Queue overlay  │
│ Capture      │               │ ├─ Vote panel     │
└──────────────┘               │ └─ Stream to      │
                               │    Twitch         │
                               └──────────────────┘
```

## VM Setup (NDI Sender)

### 1. Install NDI Tools
- Download from https://ndi.video/tools/
- Install the full "NDI Tools" package
- This includes **NDI Screen Capture** which is what we need

### 2. Run NDI Screen Capture
- Launch "NDI Screen Capture" from Start Menu
- It appears as a system tray icon
- Right-click tray icon → select which monitor/window to capture
- For best results: capture the full desktop (game + OpenClaw visible)
- The VM's screen is now broadcasting as an NDI source on the network

### 3. NDI Screen Capture Settings
- Right-click tray icon → **Settings**
- Frame rate: 60fps (or 30fps to save bandwidth)
- Audio: Capture desktop audio (game sounds)
- Name: Set to something identifiable like "D2-VM"

## Host Setup (NDI Receiver + OBS)

### 1. Install OBS Studio
- Download from https://obsproject.com/
- Install normally

### 2. Install OBS NDI Plugin
- Download from https://github.com/obs-ndi/obs-ndi/releases
- Install the plugin (follow the release instructions)
- Restart OBS after installation

### 3. Add NDI Source in OBS
- In OBS, click **+** under Sources
- Select **NDI Source**
- Name it "VM Game View"
- In properties, select the NDI source from the dropdown (should show your VM's name)
- Set bandwidth: Highest
- Enable audio if you want game sounds

### 4. OBS Scene Layout

Recommended layout for the stream:

```
┌──────────────────────────────────────────────────┐
│                                                    │
│              VM Game View (NDI)                     │
│              (fills most of screen)                 │
│                                                    │
│                                                    │
├────────────────────────┬─────────────────────────┤
│ Queue / Vote Panel     │ Stream Stats             │
│ (Browser source)       │ (Text source)            │
└────────────────────────┴─────────────────────────┘
```

#### Sources to add:
1. **NDI Source** — VM game view (main, fills ~80% of canvas)
2. **Text (GDI+)** — Stream title "Claude vs Diablo II Hell"
3. **Browser Source** — Queue/vote overlay (optional, can build a simple HTML page that reads task_queue.json)
4. **Text (GDI+)** — Live stats (kills, deaths, runs — updated by a script)

### 5. Stream Settings
- Output: x264 or NVENC, 6000 kbps for 1080p30 (or 8000 for 1080p60)
- Stream key: From your Twitch dashboard
- Audio: Include NDI audio from VM

## Testing

1. On VM: Start NDI Screen Capture
2. On host: Open OBS, add NDI source
3. Verify you see the VM's desktop in OBS
4. On VM: Launch Diablo 2 — verify the game appears in OBS
5. Test audio: game sounds should come through NDI

## Failure Handling

The key advantage of this setup: **if the VM crashes, OBS stays up**.

- Add a "Technical Difficulties" scene in OBS
- Use OBS scene switching to show a holding screen while the VM recovers
- OpenClaw can restart the VM/game autonomously — viewers see the recovery process

## Alternative: RDP as Fallback

If NDI has issues, you can use RDP as a temporary fallback:
- On host: Open RDP to VM
- In OBS: Window Capture the RDP window
- Lower quality than NDI but works immediately
- Downside: 30fps cap, compression artifacts
