#!/usr/bin/env python3
"""
M5Paper BLE Daemon
Bridges localhost HTTP → M5Paper BLE NUS (Nordic UART Service).

Endpoints:
  POST /forward   — receives Claude response text, sends to M5Paper over BLE
                    (called by m5paper_hook.py from the Stop hook)

M5Paper → Mac:
  Button choice notifications arrive over BLE NUS TX.
  The daemon pastes the choice into the focused terminal (Cmd+V + Enter).

Usage:
  ~/ble_notify_venv/bin/python3 m5paper_ble_daemon.py

Requires: ~/ble_notify_venv  (same venv as ble_notify_daemon.py)
  ~/ble_notify_venv/bin/pip install bleak aiohttp
"""

import asyncio
import datetime
import json
import logging
import os
import re
import subprocess
import sys
from pathlib import Path
from aiohttp import web
from bleak import BleakClient, BleakScanner

CHOICES_RE = re.compile(r"\[CHOICES:\s*([^\]]+)\]", re.IGNORECASE)
MAX_CHOICES = 5

CONTEXT_WINDOW = int(os.environ.get("M5P_CTX_WINDOW", "1000000"))   # default 1M (Opus 4.7 [1m])
CLAUDE_ROOT    = Path.home() / ".claude" / "projects"
STATS_INTERVAL = 30               # seconds between STAT pushes

DEVICE_NAME  = "M5Paper-Claude"
NUS_RX_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write to device
NUS_TX_UUID  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify from device
PORT         = 8764
CHUNK_SIZE   = 200   # bytes per BLE write (safe for all MTU sizes)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [m5paper] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

_client: BleakClient | None = None
_loop:   asyncio.AbstractEventLoop | None = None


# ─── BLE connection management ────────────────────────────────────────────────

def _on_disconnect(client: BleakClient) -> None:
    global _client
    log.warning("disconnected — scanning for device...")
    _client = None
    if _loop:
        asyncio.run_coroutine_threadsafe(_connect_loop(), _loop)


def _on_notify(sender, data: bytearray) -> None:
    """Button choice received from M5Paper."""
    choice = data.decode("utf-8", errors="replace").strip()
    if choice:
        log.info("← choice: %r", choice)
        _paste_into_terminal(choice)


async def _connect_loop() -> None:
    global _client
    while True:
        try:
            log.info("scanning for '%s'...", DEVICE_NAME)
            device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
            if device is None:
                log.warning("not found, retrying in 5 s")
                await asyncio.sleep(5)
                continue
            client = BleakClient(device, disconnected_callback=_on_disconnect)
            await client.connect()
            await client.start_notify(NUS_TX_UUID, _on_notify)
            _client = client
            log.info("connected to %s (%s)", DEVICE_NAME, device.address)
            await _push_time(client)
            await _push_stats(client)
            return
        except Exception as exc:
            log.error("connection failed: %s — retrying in 5 s", exc)
            await asyncio.sleep(5)


def _short(n: int) -> str:
    if n >= 1_000_000:
        return f"{n/1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n/1_000:.1f}K"
    return str(n)


def _compute_metrics() -> tuple[int, int, int]:
    """Return (ctx_size, ses_tokens, h5_tokens). ctx_size is the most recent
    assistant turn's full prompt size; ses_tokens is the cumulative billable
    token cost of the current (most recently modified) session; h5_tokens
    is the same across all sessions within the last 5h."""
    now    = datetime.datetime.now(datetime.timezone.utc)
    cutoff = now - datetime.timedelta(hours=5)

    files = list(CLAUDE_ROOT.glob("*/*.jsonl"))
    if not files:
        return 0, 0, 0
    current = max(files, key=lambda p: p.stat().st_mtime)

    ses_total = 0
    h5_total  = 0
    ctx_size  = 0

    for f in files:
        try:
            with open(f, "rb") as fp:
                for raw in fp:
                    if b'"usage"' not in raw:
                        continue
                    try:
                        d = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    if d.get("type") != "assistant":
                        continue
                    usage = d.get("message", {}).get("usage") or {}
                    inp = usage.get("input_tokens", 0)
                    cc  = usage.get("cache_creation_input_tokens", 0)
                    cr  = usage.get("cache_read_input_tokens", 0)
                    out = usage.get("output_tokens", 0)
                    turn_cost = inp + cc + out   # billable (cache_read is ~free)

                    ts_str = d.get("timestamp")
                    if ts_str:
                        try:
                            ts = datetime.datetime.fromisoformat(
                                ts_str.replace("Z", "+00:00"))
                            if ts >= cutoff:
                                h5_total += turn_cost
                        except ValueError:
                            pass

                    if f == current:
                        ses_total += turn_cost
                        ctx_size = inp + cc + cr   # latest wins
        except OSError:
            continue

    return ctx_size, ses_total, h5_total


def _format_status() -> str:
    ctx, ses, h5 = _compute_metrics()
    ctx_pct = int(ctx / CONTEXT_WINDOW * 100) if ctx else 0

    cap = int(os.environ.get("M5P_5H_CAP_TOKENS", "0") or 0)
    h5_str = f"{int(h5 / cap * 100)}%" if cap > 0 else _short(h5)

    return f"CTX {ctx_pct}%  SES {_short(ses)}  5H {h5_str}"


async def _push_stats(client: BleakClient) -> None:
    try:
        line = _format_status()
        pkt = f"STAT:{line}\n".encode()
        await client.write_gatt_char(NUS_RX_UUID, pkt, response=False)
        log.info("→ STAT %s", line)
    except Exception as exc:
        log.error("STAT push failed: %s", exc)


async def _stats_loop() -> None:
    while True:
        try:
            if _client and _client.is_connected:
                await _push_stats(_client)
        except Exception as exc:
            log.error("stats loop: %s", exc)
        await asyncio.sleep(STATS_INTERVAL)


async def _push_time(client: BleakClient) -> None:
    """Send local wall-clock time to M5Paper so its RTC matches the Mac."""
    now = datetime.datetime.now().astimezone()
    # BM8563 weekday: 0=Sun..6=Sat. Python's weekday(): Mon=0..Sun=6
    wd = (now.weekday() + 1) % 7
    pkt = f"TIME:{now.year},{now.month},{now.day},{wd}," \
          f"{now.hour},{now.minute},{now.second}\n"
    try:
        await client.write_gatt_char(NUS_RX_UUID, pkt.encode(), response=False)
        log.info("→ TIME %s", now.strftime("%Y-%m-%d %H:%M:%S %a"))
    except Exception as exc:
        log.error("TIME push failed: %s", exc)


def _split_choices(text: str) -> tuple[str, list[str]]:
    """Extract [CHOICES: A | B | C] marker; return (stripped_text, labels).
    Uses the LAST occurrence so references to '[CHOICES: ...]' earlier in
    the prose don't hijack the real marker at end-of-message.
    No marker → empty list (caller defaults to ['ACK'])."""
    matches = list(CHOICES_RE.finditer(text))
    if not matches:
        return text.rstrip(), []
    raw = matches[-1].group(1)
    labels = [s.strip() for s in raw.split("|") if s.strip()]
    labels = labels[:MAX_CHOICES]
    stripped = CHOICES_RE.sub("", text).rstrip()
    return stripped, labels


async def _push_choices(client: BleakClient, labels: list[str]) -> None:
    if not labels:
        labels = ["ACK"]
    pkt = ("CHCS:" + "|".join(labels) + "\n").encode()
    try:
        await client.write_gatt_char(NUS_RX_UUID, pkt, response=False)
        log.info("→ CHCS %s", labels)
    except Exception as exc:
        log.error("CHCS push failed: %s", exc)


async def _send_text(text: str) -> None:
    if _client is None or not _client.is_connected:
        log.warning("not connected — text dropped (%d chars)", len(text))
        return

    encoded = text.encode("utf-8")
    total   = len(encoded)

    try:
        # Announce total length so device can allocate buffer
        header = f"LEN:{total}\n".encode()
        await _client.write_gatt_char(NUS_RX_UUID, header, response=False)
        await asyncio.sleep(0.05)

        # Send in chunks
        for offset in range(0, total, CHUNK_SIZE):
            chunk = encoded[offset:offset + CHUNK_SIZE]
            await _client.write_gatt_char(NUS_RX_UUID, chunk, response=False)
            await asyncio.sleep(0.02)

        # EOT marker signals end of transmission
        await _client.write_gatt_char(NUS_RX_UUID, b"\x04", response=False)
        log.info("→ %d bytes in %d chunks", total,
                 (total + CHUNK_SIZE - 1) // CHUNK_SIZE)

    except Exception as exc:
        log.error("send failed: %s", exc)


# ─── Mac terminal paste ───────────────────────────────────────────────────────

def _paste_into_terminal(text: str) -> None:
    subprocess.run(["pbcopy"], input=text.encode(), check=True)
    script = """
tell application "System Events"
    keystroke "v" using command down
    delay 0.1
    key code 36
end tell
"""
    result = subprocess.run(["osascript", "-e", script], capture_output=True)
    if result.returncode != 0:
        log.error("osascript: %s", result.stderr.decode().strip())


# ─── HTTP handler ─────────────────────────────────────────────────────────────

async def handle_forward(req: web.Request) -> web.Response:
    text = await req.text()
    if text:
        stripped, labels = _split_choices(text)
        if _client and _client.is_connected:
            # Serialise: stats → choices → text. Avoids BLE write interleave
            # which would break the LEN/chunk framing in _send_text.
            await _push_stats(_client)
            await _push_choices(_client, labels)
        await _send_text(stripped)
    return web.Response(text="OK\n")


# ─── Main ─────────────────────────────────────────────────────────────────────

async def main() -> None:
    global _loop
    _loop = asyncio.get_running_loop()

    await _connect_loop()
    asyncio.ensure_future(_stats_loop())

    app = web.Application(client_max_size=32 * 1024)  # allow up to 32 KB response text
    app.router.add_post("/forward", handle_forward)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", PORT)
    await site.start()
    log.info("HTTP bridge ready on http://127.0.0.1:%d/forward", PORT)

    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("stopped")
        sys.exit(0)
