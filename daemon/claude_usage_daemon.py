#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Claude Controller" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).

Supports two accounts (toggled by the device's KEY button):
  - iDoctus:  Keychain service "Claude Code-credentials-idoctus"
  - Personal: Keychain service "Claude Code-credentials" (default)

Handles OAuth token refresh automatically when access tokens expire.

Also runs an HTTP server (port 27182) that receives Claude Code hook
events and forwards them to the ESP32 over BLE for sounds and permission
interaction.
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from aiohttp import web

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
SWITCH_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
EVENT_CHAR_UUID = "4c41555a-4465-7669-6365-000000000006"
PERM_RESP_CHAR_UUID = "4c41555a-4465-7669-6365-000000000007"

HTTP_PORT = 27182

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

OAUTH_TOKEN_URL = "https://claude.ai/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

ACCOUNTS = [
    {"label": "Personal", "keychain_service": "Claude Code-credentials"},
    {"label": "iDoctus", "keychain_service": "Claude Code-credentials-idoctus"},
]

CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _read_keychain_blob(service: str) -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                service,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed for '{service}' (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return out.stdout.strip()


def _write_keychain_blob(service: str, blob: str) -> bool:
    try:
        subprocess.run(
            [
                "security",
                "add-generic-password",
                "-s",
                service,
                "-a",
                getpass.getuser(),
                "-w",
                blob,
                "-U",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return True
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain write failed for '{service}': {e}")
        return False


def _parse_credential_blob(blob: str) -> dict | None:
    """Parse the Keychain blob and return {accessToken, refreshToken, expiresAt}."""
    try:
        data = json.loads(blob)
    except (json.JSONDecodeError, TypeError):
        return None
    if isinstance(data, dict):
        if "accessToken" in data:
            return data
        for v in data.values():
            if isinstance(v, dict) and "accessToken" in v:
                return v
    return None


def _token_is_expired(cred: dict) -> bool:
    expires_at = cred.get("expiresAt")
    if not expires_at:
        return False
    try:
        expires_sec = float(expires_at) / 1000.0
    except (ValueError, TypeError):
        return False
    return time.time() > (expires_sec - 300)


def _refresh_oauth_token(refresh_token: str) -> dict | None:
    """Exchange a refresh token for a new access token."""
    try:
        resp = httpx.post(
            OAUTH_TOKEN_URL,
            data={
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
                "client_id": OAUTH_CLIENT_ID,
            },
            timeout=15.0,
        )
    except httpx.HTTPError as e:
        log(f"Token refresh HTTP error: {e}")
        return None
    if resp.status_code != 200:
        log(f"Token refresh failed (HTTP {resp.status_code}): {resp.text[:200]}")
        return None
    return resp.json()


def read_token(account_idx: int) -> str | None:
    """Read and auto-refresh the OAuth token for the given account."""
    service = ACCOUNTS[account_idx]["keychain_service"]

    if sys.platform != "darwin":
        try:
            raw = CREDENTIALS_PATH.read_text()
        except OSError:
            return None
        cred = _parse_credential_blob(raw)
        return cred["accessToken"] if cred else None

    blob = _read_keychain_blob(service)
    if not blob:
        return None

    cred = _parse_credential_blob(blob)
    if not cred:
        return None

    if not _token_is_expired(cred):
        return cred["accessToken"]

    refresh_token = cred.get("refreshToken")
    if not refresh_token:
        log(f"Token expired for {ACCOUNTS[account_idx]['label']} and no refresh token")
        return None

    log(f"Token expired for {ACCOUNTS[account_idx]['label']}, refreshing...")
    new_tokens = _refresh_oauth_token(refresh_token)
    if not new_tokens:
        return None

    new_access = new_tokens.get("access_token")
    new_refresh = new_tokens.get("refresh_token", refresh_token)
    expires_in = new_tokens.get("expires_in", 28800)
    new_expires_at = int((time.time() + expires_in) * 1000)

    cred["accessToken"] = new_access
    cred["refreshToken"] = new_refresh
    cred["expiresAt"] = new_expires_at

    raw_data = json.loads(blob)
    if isinstance(raw_data, dict):
        for k, v in raw_data.items():
            if isinstance(v, dict) and "accessToken" in v:
                raw_data[k] = cred
                break
        else:
            raw_data.update(cred)
    new_blob = json.dumps(raw_data)

    if _write_keychain_blob(service, new_blob):
        log(f"Token refreshed for {ACCOUNTS[account_idx]['label']} (expires in {expires_in}s)")
    else:
        log("Token refreshed (in-memory only, Keychain write failed)")

    return new_access


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


async def poll_api(token: str, account_label: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ac": account_label,
        "ok": True,
    }
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        self.switch_requested = asyncio.Event()
        self.perm_response: asyncio.Future | None = None

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    def _on_switch(self, _char, _data: bytearray) -> None:
        log("Account switch requested by device")
        self.switch_requested.set()

    def _on_perm_resp(self, _char, data: bytearray) -> None:
        resp = data.decode("utf-8", errors="replace")
        log(f"Permission response from device: {resp}")
        if self.perm_response and not self.perm_response.done():
            self.perm_response.set_result(resp)

    async def setup_subscriptions(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        try:
            await self.client.start_notify(SWITCH_CHAR_UUID, self._on_switch)
        except (BleakError, ValueError) as e:
            log(f"Switch subscription unavailable: {e}")
        try:
            await self.client.start_notify(PERM_RESP_CHAR_UUID, self._on_perm_resp)
        except (BleakError, ValueError) as e:
            log(f"Permission response subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False

    async def send_event(self, event_json: str) -> bool:
        """Send an event to the ESP32 via the EVENT characteristic."""
        try:
            await self.client.write_gatt_char(
                EVENT_CHAR_UUID, event_json.encode(), response=False
            )
            log(f"Event sent: {event_json}")
            return True
        except BleakError as e:
            log(f"Event write failed: {e}")
            return False

    async def send_permission_request(self, tool: str, cmd: str, timeout: float = 30.0) -> str:
        """Send a permission request and wait for the device's response."""
        event = json.dumps({"ev": "perm", "tool": tool, "cmd": cmd[:120]},
                           separators=(",", ":"))
        self.perm_response = asyncio.get_event_loop().create_future()
        if not await self.send_event(event):
            return '{"resp":"deny"}'
        try:
            resp = await asyncio.wait_for(self.perm_response, timeout=timeout)
            return resp
        except asyncio.TimeoutError:
            log("Permission response timeout")
            return '{"resp":"deny"}'
        finally:
            self.perm_response = None


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    log(f"Connecting to {address}...")
    client = BleakClient(address, timeout=15.0)
    try:
        await asyncio.wait_for(client.connect(), timeout=20.0)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_subscriptions()

    global _active_session
    _active_session = session

    account_idx = 0
    last_poll = 0.0
    used_successfully = False
    # Activity inference: track session % per account to detect working/idle
    prev_session_pct: dict[int, int | None] = {}  # account_idx -> last seen s%
    flat_polls: dict[int, int] = {}  # account_idx -> consecutive unchanged polls
    FLAT_THRESHOLD = 2  # polls with same s% before inferring idle
    try:
        while client.is_connected and not stop_event.is_set():
            if session.switch_requested.is_set():
                session.switch_requested.clear()
                account_idx = (account_idx + 1) % len(ACCOUNTS)
                label = ACCOUNTS[account_idx]["label"]
                log(f"Switched to account: {label}")
                last_poll = 0.0

            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                label = ACCOUNTS[account_idx]["label"]
                token = read_token(account_idx)
                if not token:
                    log(f"No token for {label}; skipping poll")
                else:
                    payload = await poll_api(token, label)
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True

                            # --- Activity inference from usage delta ---
                            cur_s = payload.get("s")
                            prev_s = prev_session_pct.get(account_idx)
                            if prev_s is not None and cur_s is not None:
                                if cur_s != prev_s:
                                    flat_polls[account_idx] = 0
                                    await session.send_event('{"ev":"working"}')
                                    log(f"Activity: working (s% {prev_s}→{cur_s})")
                                else:
                                    flat_polls[account_idx] = flat_polls.get(account_idx, 0) + 1
                                    if flat_polls[account_idx] == FLAT_THRESHOLD:
                                        await session.send_event('{"ev":"stop"}')
                                        log(f"Activity: idle (s% flat at {cur_s} for {flat_polls[account_idx]} polls)")
                            prev_session_pct[account_idx] = cur_s

            try:
                done, _ = await asyncio.wait(
                    [
                        asyncio.create_task(session.refresh_requested.wait()),
                        asyncio.create_task(session.switch_requested.wait()),
                        asyncio.create_task(asyncio.sleep(TICK)),
                    ],
                    return_when=asyncio.FIRST_COMPLETED,
                )
                for t in done:
                    t.result()
            except asyncio.CancelledError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    _active_session = None
    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


# ---- HTTP server for Claude Code hooks ----
# Global reference so HTTP handlers can reach the active BLE session.
_active_session: Session | None = None


async def handle_event(request: web.Request) -> web.Response:
    """POST /event — fire-and-forget sound events."""
    if _active_session is None:
        return web.json_response({"error": "no BLE session"}, status=503)
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "bad json"}, status=400)

    ev = body.get("event", "")

    # Permission dialog: includes tool name and command preview
    if ev == "perm_dialog":
        tool = body.get("tool", "Unknown")
        cmd = body.get("cmd", "")[:120]
        payload = json.dumps(
            {"ev": "perm", "tool": tool, "cmd": cmd}, separators=(",", ":")
        )
        ok = await _active_session.send_event(payload)
        return web.json_response({"ok": ok})

    event_map = {
        "stop": "stop",
        "stop_failure": "fail",
        "permission_prompt": "pprompt",
        "idle_prompt": "idle",
        "task_completed": "done",
    }
    short = event_map.get(ev)
    if not short:
        return web.json_response({"error": f"unknown event: {ev}"}, status=400)

    payload = json.dumps({"ev": short}, separators=(",", ":"))
    ok = await _active_session.send_event(payload)
    return web.json_response({"ok": ok})


async def handle_permission(request: web.Request) -> web.Response:
    """POST /permission — blocking: waits for device response."""
    if _active_session is None:
        return web.json_response({"resp": "deny", "error": "no BLE session"}, status=503)
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"resp": "deny", "error": "bad json"}, status=400)

    tool = body.get("tool", "Unknown")
    cmd = body.get("command", "")
    resp_raw = await _active_session.send_permission_request(tool, cmd)
    try:
        resp = json.loads(resp_raw)
    except (json.JSONDecodeError, TypeError):
        resp = {"resp": "deny"}
    return web.json_response(resp)


async def start_http_server() -> web.AppRunner:
    app = web.Application()
    app.router.add_post("/event", handle_event)
    app.router.add_post("/permission", handle_permission)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", HTTP_PORT)
    await site.start()
    log(f"HTTP server listening on http://127.0.0.1:{HTTP_PORT}")
    return runner


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Accounts: {', '.join(a['label'] for a in ACCOUNTS)}")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # Start HTTP server for Claude Code hooks
    http_runner = await start_http_server()

    backoff = 1
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(address, stop_event)
        if not ok:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1

    await http_runner.cleanup()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
