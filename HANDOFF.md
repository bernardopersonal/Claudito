# Clawdmeter — Session Handoff

## What Changed (this session + previous)

This fork adds **sound effects**, **event system**, and **interactive permission control** to the Clawdmeter. The upstream repo is `HermannBjorgvin/Clawdmeter`.

### New Files

| File | Purpose |
|------|---------|
| `firmware/src/hal/audio_hal.h` | Audio HAL interface (init, write, volume, mute) |
| `firmware/src/boards/waveshare_amoled_216_c6/audio.cpp` | ES8311 codec + I2S driver for C6 board |
| `firmware/src/boards/waveshare_amoled_216/audio.cpp` | Stub (no audio on this board) |
| `firmware/src/boards/waveshare_amoled_18/audio.cpp` | Stub (no audio on this board) |
| `firmware/src/sound.h` / `sound.cpp` | FreeRTOS sound player (queue-based, non-blocking) |
| `firmware/src/sound_data.h` | **GENERATED** — 6 PCM arrays from WAV files (~1069 KB, 44100 Hz mono) |
| `firmware/src/event.h` / `event.cpp` | Event dispatcher: BLE events -> sounds + permission dialog (LVGL overlay with Once/Always/Deny buttons that send HID keystrokes) |
| `firmware/src/idoctus_logo.h` | iDoctus account logo for UI |
| `daemon/claude-hook.sh` | Shell script called by Claude Code hooks, sends events to daemon HTTP server |
| `daemon/hooks-config.json` | Reference copy of hooks config |
| `tools/generate_sounds.py` | Converts WAV files to C header with PROGMEM arrays |
| `tools/test_sounds.py` | Serial test script to play all 6 sounds sequentially |

### Modified Files

| File | Changes |
|------|---------|
| `firmware/src/ble.cpp` | Added EVENT_CHAR (0006) + PERM_RESP_CHAR (0007), EventCallbacks, `ble_has_event()`, `ble_get_event()`, `ble_send_permission_response()` |
| `firmware/src/ble.h` | Added event/permission response function declarations |
| `firmware/src/main.cpp` | Added `sound_init()`, `event_init()`, `event_tick()`, serial test commands `snd0`-`snd5` |
| `firmware/src/ui.cpp` | Traffic-light bar colors (green/yellow/orange/red at 40/70/90%), idle state display ("Stopped", "Token hungry", etc.), `ui_set_activity()` |
| `firmware/src/ui.h` | Added `activity_state_t` enum and `ui_set_activity()` |
| `firmware/src/theme.h` | Added `THEME_YELLOW` and `THEME_ORANGE` colors |
| `firmware/src/data.h` | May have minor changes |
| `daemon/claude_usage_daemon.py` | Added HTTP server (:27182), event forwarding via BLE, `perm_dialog` event type, permission response handling |

### Claude Code Hooks (installed in ~/.claude/settings.json)

```json
{
  "hooks": {
    "Stop": [{ "matcher": "", "hooks": [{ "type": "command", "command": ".../claude-hook.sh stop", "async": true }] }],
    "Notification": [{ "matcher": "", "hooks": [{ "type": "command", "command": ".../claude-hook.sh notification", "async": true }] }],
    "TaskCompleted": [{ "matcher": "", "hooks": [{ "type": "command", "command": ".../claude-hook.sh task_completed", "async": true }] }],
    "PermissionRequest": [{ "matcher": "", "hooks": [{ "type": "command", "command": ".../claude-hook.sh permission_request", "async": true }] }]
  }
}
```

## Architecture

```
Claude Code/Cowork
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
ESP32-C6 (NimBLE)
  |
  +-> event.cpp dispatches to:
  |     sound.cpp (FreeRTOS task, streams PCM via I2S -> ES8311 codec)
  |     Permission dialog (LVGL overlay with touch buttons)
  |       -> HID keyboard press (Enter/Tab+Enter/Escape) back to Mac
  |
  +-> ui.cpp shows activity state (Working spinner / Idle / Waiting)
```

## Sound Mapping

| Event | Sound ID | File | Duration |
|-------|----------|------|----------|
| Stop | SND_STOP | SND_STOP.wav | 2.97s |
| Stop Failure | SND_STOP_FAILURE | SND_STOP_FAILURE.wav | 1.48s |
| Permission Request | SND_PERMISSION_REQUEST | SND_PERMISSION_REQUEST.wav | 2.02s |
| Permission Prompt | SND_PERMISSION_PROMPT | SND_PERMISSION_PROMPT.wav | 2.97s |
| Idle Prompt | SND_IDLE_PROMPT | SND_IDLE_PROMPT.wav | 1.48s |
| Task Completed | SND_TASK_COMPLETED | SND_TASK_COMPLETEDwav.wav | 1.48s |

All sounds are 44100 Hz, 16-bit mono PCM stored in flash via PROGMEM.

## How to Regenerate sound_data.h

```bash
python3.13 tools/generate_sounds.py /path/to/wav/files firmware/src/sound_data.h
```

WAV files are NOT in the repo (too large). Keep them in a local folder.

## How to Build & Flash

```bash
cd firmware
~/.platformio/penv/bin/pio run -e waveshare_amoled_216_c6           # build
~/.platformio/penv/bin/pio run -e waveshare_amoled_216_c6 -t upload  # flash
```

Serial port: `/dev/cu.usbmodem3101` (may change after reflash).

## How to Run the Daemon

```bash
python3.13 daemon/claude_usage_daemon.py
```

Requires: `bleak`, `httpx`, `aiohttp` (install via `pip3.13 install bleak httpx aiohttp`).

## Known Issues / TODO

1. **Permission HID keys untested end-to-end** — Enter/Tab+Enter/Escape mapping for Claude Code permission prompt needs verification in a session where permissions aren't pre-approved.
2. **HID requires focus** — Device buttons send HID keystrokes to the focused Mac app. Won't work for background cowork sessions.
3. **sound_data.h is 1+ MB** — Consider .gitignore'ing it and keeping WAVs + generate script only.
4. **macOS GATT cache** — If new BLE characteristics aren't found after firmware changes, add `NimBLEDevice::deleteAllBonds()` temporarily in `ble_init()`, flash, reconnect, then remove it.
5. **Daemon as launchd service** — `com.user.claude-usage-daemon.plist` exists but not installed. User runs manually.

## Git Status

- Branch: `main` (forked from HermannBjorgvin/Clawdmeter)
- Remote: `origin` -> `https://github.com/HermannBjorgvin/Clawdmeter.git`
- 8 modified files + 14 untracked new files
- No commits made for these changes yet
