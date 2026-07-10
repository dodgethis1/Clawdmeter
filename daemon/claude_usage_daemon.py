#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import http.server
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0   # CoreBluetooth connect can hang forever without this
WATCHDOG_SECS = 600      # no successful send in this long -> exit, launchd relaunches

# Watchdog heartbeat: updated on every successful BLE write. A list so the
# watchdog task and connect loop share it without global statements.
_last_alive = [time.time()]

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

API_URL = "https://api.anthropic.com/v1/messages"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

# OAuth refresh — endpoint and client id used by Claude Code itself
# (extracted from the CLI bundle; the daemon refreshes the same stored
# credentials Claude Code uses so both stay in sync).
TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

# Sentinel returned by poll_api on 401/403 so the caller can refresh + retry.
AUTH_ERROR = object()
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


# ---------------------------------------------------------------------------
# HTTP usage endpoint (for the Puckmeter hub).
# Serves the latest polled usage payload on GET /usage. Set
# CLAWDMETER_HTTP_PORT=0 to disable.
# ---------------------------------------------------------------------------
HTTP_PORT = int(os.environ.get("CLAWDMETER_HTTP_PORT", "8788"))

_last_usage_lock = threading.Lock()
_last_usage: dict = {"ts": 0.0, "payload": None}


def _store_usage(payload: dict) -> None:
    with _last_usage_lock:
        _last_usage["ts"] = time.time()
        _last_usage["payload"] = payload


class _UsageHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/usage":
            self.send_response(404)
            self.end_headers()
            return
        with _last_usage_lock:
            ts = _last_usage["ts"]
            body = json.dumps({
                "ts": ts,
                "age_s": round(time.time() - ts, 1) if ts else None,
                "usage": _last_usage["payload"],
            }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass  # keep daemon log clean


def start_http_server() -> None:
    if HTTP_PORT <= 0:
        return
    try:
        srv = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), _UsageHandler)
    except OSError as e:
        log(f"HTTP usage endpoint disabled: {e}")
        return
    threading.Thread(target=srv.serve_forever, daemon=True, name="usage-http").start()
    log(f"HTTP usage endpoint on :{HTTP_PORT}/usage")


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
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
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def _read_blob() -> str | None:
    """Read the raw credentials blob (Keychain on macOS, file on Linux)."""
    if sys.platform == "darwin":
        try:
            out = subprocess.run(
                ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE,
                 "-a", getpass.getuser(), "-w"],
                check=True, capture_output=True, text=True, timeout=10,
            )
        except (subprocess.CalledProcessError, FileNotFoundError,
                subprocess.TimeoutExpired) as e:
            log(f"Keychain read failed: {e}")
            return None
        return out.stdout
    try:
        return CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None


def _parse_credentials(blob: str):
    """Return (outer, oauth) dicts, where oauth holds accessToken/refreshToken."""
    try:
        data = json.loads(blob.strip())
    except json.JSONDecodeError:
        return None, None
    if not isinstance(data, dict):
        return None, None
    if isinstance(data.get("accessToken"), str):
        return data, data
    for v in data.values():
        if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
            return data, v
    return None, None


def _write_credentials(outer: dict) -> bool:
    """Persist the (updated) credentials blob back where Claude Code keeps it."""
    blob = json.dumps(outer)
    if sys.platform == "darwin":
        try:
            subprocess.run(
                ["security", "add-generic-password", "-U", "-s", KEYCHAIN_SERVICE,
                 "-a", getpass.getuser(), "-w", blob],
                check=True, capture_output=True, text=True, timeout=10,
            )
            return True
        except (subprocess.CalledProcessError, FileNotFoundError,
                subprocess.TimeoutExpired) as e:
            log(f"Keychain write failed: {e}")
            return False
    try:
        CREDENTIALS_PATH.write_text(blob)
        return True
    except OSError as e:
        log(f"Error writing credentials: {e}")
        return False


async def ensure_token(force: bool = False) -> str | None:
    """Return a valid access token, refreshing it via OAuth when expired.

    Reads the same credential store Claude Code uses. If the access token
    is expired (or ``force`` is set, e.g. after a 401), exchange the
    refreshToken at TOKEN_URL and write the rotated credentials back so
    Claude Code and the daemon stay in sync.
    """
    blob = _read_blob()
    if blob is None:
        return None
    outer, oauth = _parse_credentials(blob)
    if oauth is None:
        return _extract_access_token(blob)  # unexpected shape — old behavior

    expires_at_ms = oauth.get("expiresAt") or 0
    if not force and time.time() * 1000 < expires_at_ms - 60_000:
        return oauth.get("accessToken")

    refresh = oauth.get("refreshToken")
    if not refresh:
        log("Token expired and no refreshToken present")
        return oauth.get("accessToken")

    log("Access token expired - refreshing via OAuth")
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(TOKEN_URL, json={
                "grant_type": "refresh_token",
                "refresh_token": refresh,
                "client_id": OAUTH_CLIENT_ID,
            })
    except httpx.HTTPError as e:
        log(f"Token refresh failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"Token refresh HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    try:
        tok = resp.json()
    except ValueError:
        log("Token refresh returned non-JSON body")
        return None
    if not tok.get("access_token"):
        log("Token refresh response missing access_token")
        return None

    oauth["accessToken"] = tok["access_token"]
    if tok.get("refresh_token"):
        oauth["refreshToken"] = tok["refresh_token"]
    if tok.get("expires_in"):
        oauth["expiresAt"] = int((time.time() + tok["expires_in"]) * 1000)
    if _write_credentials(outer):
        log("Refreshed token stored")
    else:
        log("Refreshed token NOT stored (using in-memory token this run)")
    return oauth["accessToken"]


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
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


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


async def poll_api(token: str) -> dict | None:
    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": API_HEADERS_TEMPLATE["anthropic-beta"],
        "User-Agent": API_HEADERS_TEMPLATE["User-Agent"],
    }
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code} (auth): {resp.text[:200]}")
        return AUTH_ERROR
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    try:
        body = resp.json()
    except ValueError as e:
        log(f"API returned non-JSON body: {e}")
        return None

    now = time.time()

    def reset_minutes(iso: str | None) -> int:
        if not iso:
            return -1
        try:
            ts = datetime.fromisoformat(iso).timestamp()
        except (TypeError, ValueError):
            return -1
        mins = (ts - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    session = weekly = None
    scoped: list[dict] = []
    for lim in body.get("limits") or []:
        kind = lim.get("kind")
        if kind == "session":
            session = lim
        elif kind == "weekly_all":
            weekly = lim
        elif kind == "weekly_scoped":
            scoped.append(lim)

    def pct(lim: dict | None) -> int:
        return int(lim.get("percent") or 0) if lim else 0

    def rst(lim: dict | None) -> int:
        return reset_minutes(lim.get("resets_at")) if lim else -1

    severity = (session or {}).get("severity", "unknown")
    local_now = datetime.now()
    payload = {
        "s": pct(session),
        "sr": rst(session),
        "w": pct(weekly),
        "wr": rst(weekly),
        "st": "allowed" if severity in ("normal", "warning") else "limited",
        "tm": local_now.hour * 60 + local_now.minute,
        "ok": True,
    }
    # Per-model weekly buckets — generic: whatever scoped limits the API
    # reports get forwarded (Fable until retirement, Opus/Sonnet if/when
    # they appear). Capped to keep the BLE payload small.
    models = []
    for lim in scoped[:4]:
        scope = lim.get("scope") or {}
        name = ((scope.get("model") or {}).get("display_name") or "Model")
        models.append({"n": name[:10], "p": pct(lim), "r": rst(lim)})
    if models:
        payload["m"] = models
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Hard cap the connect: CoreBluetooth has been observed to sit in
        # connect() indefinitely when the OS holds a stale peripheral handle.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e or 'timed out'}")
        try:
            await client.disconnect()
        except (BleakError, Exception):
            pass
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                last_poll = time.time()
                # Polling now lives in poll_loop() — the BLE session just
                # forwards the latest stored payload to the legacy device.
                with _last_usage_lock:
                    payload = _last_usage["payload"]
                if payload is not None:
                    if await session.write_payload(payload):
                        used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def poll_loop(stop_event: asyncio.Event) -> None:
    """Poll the usage API on an independent clock. The HTTP endpoint (and
    any BLE client) consume the stored result — a BLE device is optional."""
    while not stop_event.is_set():
        token = await ensure_token()
        if token:
            payload = await poll_api(token)
            if payload is AUTH_ERROR:
                # Stored token rejected — force a refresh and retry once.
                token = await ensure_token(force=True)
                payload = await poll_api(token) if token else None
                if payload is AUTH_ERROR:
                    payload = None
            if payload is not None:
                _store_usage(payload)
                _last_alive[0] = time.time()   # watchdog keys on poll success
        else:
            log("No token; skipping poll")
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=POLL_INTERVAL)
        except asyncio.TimeoutError:
            pass


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
    log(f"Poll interval: {POLL_INTERVAL}s")
    start_http_server()

    # Watchdog: if no successful API poll in WATCHDOG_SECS, assume we're
    # wedged and exit hard. launchd's KeepAlive relaunches us with a clean
    # slate. (Re-keyed from BLE sends to poll success — BLE is optional now.)
    async def _watchdog() -> None:
        while not stop_event.is_set():
            await asyncio.sleep(30)
            stale = time.time() - _last_alive[0]
            if stale > WATCHDOG_SECS:
                log(f"Watchdog: no successful poll in {int(stale)}s - exiting for relaunch")
                os._exit(1)

    asyncio.get_running_loop().create_task(_watchdog())
    asyncio.get_running_loop().create_task(poll_loop(stop_event))

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
