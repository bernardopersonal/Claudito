# Claudito

A fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) that adds **sound effects**, an **event system**, **Claude Code hooks integration**, and **multi-account support** to the ESP32 desk-side Claude Code usage monitor.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

## What this fork adds

### Sound effects
Six PCM sounds played through the ES8311 codec on the [ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm) board via I2S. A FreeRTOS task streams audio from flash without blocking the UI. Boards without audio hardware get silent stubs.

| Event | Sound |
|-------|-------|
| Claude stops responding | Chime |
| Stop failure | Alert |
| Permission request | Dialog tone |
| Permission prompt | Attention tone |
| Idle prompt | Nudge |
| Task completed | Fanfare |

### Event system
A new BLE characteristic (`EVENT_CHAR`, UUID `...0006`) receives event payloads from the daemon. `event.cpp` dispatches them to the sound player and, for permission requests, shows an LVGL overlay dialog with **Once / Always / Deny** touch buttons. Tapping a button sends an HID keystroke (Enter / Tab+Enter / Escape) back to the Mac to answer Claude Code's permission prompt directly from the device.

### Claude Code hooks integration
A shell script (`daemon/claude-hook.sh`) is called by Claude Code hooks on `Stop`, `Notification`, `TaskCompleted`, and `PermissionRequest` events. It POSTs to the daemon's HTTP server (localhost:27182), which forwards events to the ESP32 over BLE.

### Multi-account support
The Python daemon (`daemon/claude_usage_daemon.py`) supports two Claude Code accounts, toggled by the device's KEY button. Each account reads its OAuth credential from the macOS Keychain at runtime (no tokens stored in code). Configure the Keychain service names in the `ACCOUNTS` list inside the daemon script.

### UI enhancements
- Traffic-light usage bar: green below 40%, yellow at 40%, orange at 70%, red at 90%
- Activity state display: shows "Working", "Idle", "Waiting" etc. based on Claude Code hooks

## Architecture

```
Claude Code
  |
  | hooks (async, non-blocking)
  v
claude-hook.sh
  |
  | HTTP POST to localhost:27182
  v
claude_usage_daemon.py (aiohttp + bleak)
  |
  | BLE GATT write to EVENT_CHAR (0006)
  v
ESP32 (NimBLE)
  |
  +-> event.cpp dispatches to:
  |     sound.cpp (FreeRTOS task, I2S -> ES8311 codec)
  |     Permission dialog (LVGL overlay with touch buttons)
  |       -> HID keystroke back to Mac
  |
  +-> ui.cpp shows activity state + usage meters
```

## Supported boards

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm) — original, no audio
- [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm) — audio via ES8311 codec
- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm) — compact, no audio

See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md) for adding new boards.

## Setup

### Prerequisites

- macOS or Linux
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Python 3.10+ with `bleak`, `httpx`, `aiohttp`
- Claude Code with an active subscription

### Flash the firmware

```bash
# macOS
./flash-mac.sh waveshare_amoled_216_c6                       # auto-detects USB port
./flash-mac.sh waveshare_amoled_216   /dev/cu.usbmodem1101   # explicit port

# Linux
./flash.sh waveshare_amoled_216_c6
```

### Pair the device

Open **System Settings > Bluetooth** (macOS) and connect to "Clawdmeter". The daemon discovers it on its next scan.

### Run the daemon

```bash
pip3 install bleak httpx aiohttp
python3 daemon/claude_usage_daemon.py
```

The daemon reads OAuth credentials from the macOS Keychain at runtime. No tokens are stored in any file in this repository. On Linux it falls back to `~/.claude/.credentials.json`.

### Install Claude Code hooks

Copy the hooks configuration from `daemon/hooks-config.json` into your `~/.claude/settings.json`, replacing `<path-to-claudito>` with the absolute path to your clone:

```json
{
  "hooks": {
    "Stop": [{ "matcher": "", "hooks": [{ "type": "command", "command": "<path-to-claudito>/daemon/claude-hook.sh stop", "async": true }] }],
    "Notification": [{ "matcher": "", "hooks": [{ "type": "command", "command": "<path-to-claudito>/daemon/claude-hook.sh notification", "async": true }] }],
    "TaskCompleted": [{ "matcher": "", "hooks": [{ "type": "command", "command": "<path-to-claudito>/daemon/claude-hook.sh task_completed", "async": true }] }]
  }
}
```

### Configure accounts

Edit the `ACCOUNTS` list in `daemon/claude_usage_daemon.py` to match your Keychain service names. The default setup supports two accounts — adjust to your needs.

## Regenerating sound data

WAV source files are not included in the repo. To regenerate `firmware/src/sound_data.h` from your own WAV files:

```bash
python3 tools/generate_sounds.py /path/to/wav/files firmware/src/sound_data.h
```

All sounds must be 44100 Hz, 16-bit mono WAV.

## Screens

|              Splash               |              Usage              |                Bluetooth                |
| :-------------------------------: | :-----------------------------: | :-------------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | ![Bluetooth](screenshots/bluetooth.png) |

The device boots into the splash and stays there until you press the middle (PWR) button, which cycles between Usage and Bluetooth. Tap the screen to flip back to the splash.

## Physical buttons

Buttons vary by board. The C6 has all three; the 1.8" only has Primary + PWR.

| Button | C6 GPIO | 2.16 GPIO | 1.8 GPIO | Function |
|--------|---------|-----------|----------|----------|
| **Primary** (BOOT) | GPIO 9 | GPIO 0 | GPIO 0 | Send HID Space keystroke |
| **PWR** (middle) | AXP PKEY | AXP PKEY | XCA9554 EXIO4 | Cycle screens; on splash, cycle animations |
| **Secondary** (KEY) | GPIO 10 | GPIO 18 | — | Switch BLE account (toggles between configured accounts) |

## BLE protocol

Custom GATT service alongside standard HID keyboard:

| Characteristic | UUID | Direction |
|----------------|------|-----------|
| Data Service | `4c41555a-...0001` | — |
| RX (usage data) | `4c41555a-...0002` | host → device |
| TX (ack) | `4c41555a-...0003` | device → host |
| REQ (refresh) | `4c41555a-...0004` | device → host |
| Account switch | `4c41555a-...0005` | device → host |
| **Event** | `4c41555a-...0006` | host → device |
| **Permission response** | `4c41555a-...0007` | device → host |

The last two are new in this fork.

## Compatibility

Hooks-based features (sounds, permission dialog, activity state) depend on Claude Code CLI hooks. Not all Claude products fire them:

| Product | Usage % | Working/idle | Sounds | Permission dialog |
|---------|---------|-------------|--------|-------------------|
| **Claude Code CLI** | ✅ polling | ✅ hooks + polling fallback | ✅ hooks | ✅ hooks |
| **Cowork** | ✅ polling | ✅ polling inference only | ❌ | ❌ |
| **Claude Chat (desktop)** | ✅ polling | ✅ polling inference only | ❌ | ❌ |

For Cowork and Chat, the daemon infers working/idle from session % changes between polls (~60s granularity). Hooks give near-instant feedback in Claude Code CLI.

## Known issues

- Permission HID keys (Enter/Tab+Enter/Escape) require the Claude Code window to have focus on the Mac. If the permission was already handled on the computer, device buttons dismiss the dialog silently without sending keystrokes
- `sound_data.h` is ~1 MB — consider `.gitignore`-ing it and regenerating from WAVs
- macOS may cache stale GATT characteristics after firmware changes — temporarily add `NimBLEDevice::deleteAllBonds()` in `ble_init()`, reflash, reconnect, then remove it

## Credits

- Original project: [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
- Pixel-art Clawd animations by [@amaanbuilds](https://x.com/amaanbuilds) from [claudepix.vercel.app](https://claudepix.vercel.app)
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT)
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below

## Licensing gray area warning

This repository inherits the upstream's situation: it uses Anthropic brand fonts and the copyrighted Clawd mascot without explicit permission. The code itself is non-proprietary but is not released under a copyleft license due to these proprietary assets. **Be aware of this if you fork or copy.**
