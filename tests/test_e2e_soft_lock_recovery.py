"""End-to-end test: the server auto-recovers a human who never sends a pass.

This is the regression test for the Christian/Sanjaya soft-lock bug:
  - autoPass is enabled client-side (and now server-side via room flag too).
  - On a real device, the JS event loop may be paused (mobile background tab,
    incoming phone call). The client's auto-pass timer never fires.
  - Previously the server would never act on a human seat, so the round
    soft-locked: nobody else could draw because the claim window was open.
  - Now the server runs a background idle worker. After MAHJONG_IDLE_ACT_MS
    elapses with an unanswered pending action, it submits Pass (or a safe AI
    fallback) on the human's behalf without flipping the seat to AI.

Test setup:
  - Spawns the server on port 18095 with MAHJONG_IDLE_ACT_MS=400 and a long
    takeover threshold so we test ONLY the auto-pass-on-behalf path.
  - Claims seat 0, connects a WS, then deliberately stays silent on every
    snapshot for seat 0.
  - Asserts the room version advances on its own across multiple snapshots
    (i.e., the game continues even though the human never acts).

Usage:
    python tests\\test_e2e_soft_lock_recovery.py
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import time
import urllib.request

import requests
import websockets

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_EXE = os.path.join(ROOT, "build-web", "Release", "mahjong_web_server.exe")
WEB_DIR = os.path.join(ROOT, "web")
PORT = 18095
BASE = f"http://localhost:{PORT}"
WS_URL = f"ws://localhost:{PORT}/ws"


def _wait_for_server(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"{BASE}/api/health", timeout=1).read()
            return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError(f"server not up on {BASE}")


async def _drain_snapshots(ws, duration_s, on_snapshot):
    """Pump WS messages for `duration_s` seconds. Never sends anything back.

    Calls `on_snapshot(snapshot)` for every snapshot frame. Tolerates pongs
    and unrelated frames. Bubbles up server-side error frames so the test
    fails loudly on unexpected codes.
    """
    deadline = time.time() + duration_s
    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
        except asyncio.TimeoutError:
            break
        msg = json.loads(raw)
        if msg.get("type") == "snapshot":
            on_snapshot(msg["snapshot"])
        elif msg.get("type") == "error":
            raise RuntimeError(f"server pushed error: {msg}")


async def _run():
    # 1) Create a room and claim seat 0 as the human.
    room = requests.post(f"{BASE}/api/rooms", json={"seed": "soft-lock-test"},
                         timeout=5).json()
    code = room["roomCode"]
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = requests.post(
        f"{BASE}/api/rooms/{code}/seats/0",
        json={"token": seat0["token"], "displayName": "Christian"},
        timeout=5,
    ).json()
    session_token = claim["sessionToken"]

    versions_seen = []

    def record(snap):
        v = snap.get("version")
        if v is not None and (not versions_seen or versions_seen[-1] != v):
            versions_seen.append(v)

    # 2) Open a WS, send hello, then stay completely silent.
    async with websockets.connect(WS_URL) as ws:
        await ws.send(json.dumps({
            "type": "hello",
            "roomCode": code,
            "seatIndex": 0,
            "sessionToken": session_token,
        }))
        # Pump frames for ~6 seconds. With idleActMs=400 and three AI seats,
        # the game should keep advancing entirely on its own.
        await _drain_snapshots(ws, duration_s=6.0, on_snapshot=record)

    # 3) Assert the game actually advanced without us ever acting.
    assert len(versions_seen) >= 3, (
        f"Only saw {len(versions_seen)} unique snapshot versions while idle "
        f"(expected several as AI plays and server auto-passes for us): "
        f"{versions_seen[:10]}"
    )
    span = versions_seen[-1] - versions_seen[0]
    assert span >= 3, (
        f"Snapshot versions barely moved while we were idle "
        f"({versions_seen[0]} -> {versions_seen[-1]}, span={span}). "
        f"The server is probably not auto-recovering the idle human seat."
    )
    print(f"OK: saw versions {versions_seen[0]} -> {versions_seen[-1]} "
          f"({len(versions_seen)} unique) with the human seat completely idle.")


def main():
    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["MAHJONG_WEB_DIR"] = WEB_DIR
    env["MAHJONG_IDLE_ACT_MS"] = "400"
    env["MAHJONG_IDLE_TAKEOVER_MS"] = "120000"  # don't trigger takeover here
    env["MAHJONG_ROOM_TTL_FINISHED_SECONDS"] = "86400"
    env["MAHJONG_ROOM_TTL_ACTIVE_SECONDS"] = "86400"
    proc = subprocess.Popen(
        [SERVER_EXE], cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        _wait_for_server()
        asyncio.run(_run())
    finally:
        try: proc.terminate()
        except Exception: pass
        try: proc.wait(timeout=5)
        except Exception:
            try: proc.kill()
            except Exception: pass


if __name__ == "__main__":
    try:
        main()
    except AssertionError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        sys.exit(1)
