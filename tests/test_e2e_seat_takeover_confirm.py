"""End-to-end test: taking over an actively held seat requires confirmation.

Verifies:
  - The first POST /api/rooms/<code>/seats/<n> succeeds and binds a session.
  - While that session is connected via WS, a second POST to the same seat
    (without forceTakeover) returns 409 with body {"error":"seat_in_use"}.
  - A retry with forceTakeover=true returns 200, gives a NEW session token,
    and the original WS connection is pushed an error frame with code
    'session_invalidated' so the old client can demote itself to the lobby.

Spawns its own server on port 18096.

Usage:
    python tests\\test_e2e_seat_takeover_confirm.py
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
PORT = 18096
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


async def _run():
    # 1) Create a room and claim seat 0.
    room = requests.post(f"{BASE}/api/rooms", json={"seed": "takeover-test"},
                         timeout=5).json()
    code = room["roomCode"]
    seat0_link = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)

    claim1 = requests.post(
        f"{BASE}/api/rooms/{code}/seats/0",
        json={"token": seat0_link["token"], "displayName": "Original"},
        timeout=5,
    )
    assert claim1.status_code == 200, f"first claim failed: {claim1.status_code} {claim1.text}"
    session1 = claim1.json()["sessionToken"]

    # 2) Connect the WS for the original session.
    ws1 = await websockets.connect(WS_URL)
    await ws1.send(json.dumps({
        "type": "hello",
        "roomCode": code,
        "seatIndex": 0,
        "sessionToken": session1,
    }))
    # Drain initial welcome + snapshot so the seat is registered as live.
    saw_welcome = False
    for _ in range(5):
        try:
            raw = await asyncio.wait_for(ws1.recv(), timeout=2)
        except asyncio.TimeoutError:
            break
        m = json.loads(raw)
        if m.get("type") == "welcome":
            saw_welcome = True
        if m.get("type") == "snapshot":
            break
    assert saw_welcome, "did not receive welcome on ws1"

    # 3) Attempt a silent takeover. Should be rejected with 409.
    rej = requests.post(
        f"{BASE}/api/rooms/{code}/seats/0",
        json={"token": seat0_link["token"], "displayName": "Intruder"},
        timeout=5,
    )
    assert rej.status_code == 409, f"expected 409 seat_in_use, got {rej.status_code} {rej.text}"
    body = rej.json()
    assert body.get("error") == "seat_in_use", f"unexpected body: {body}"

    # 4) Retry with forceTakeover=true. Should succeed, return a new session.
    ok = requests.post(
        f"{BASE}/api/rooms/{code}/seats/0",
        json={"token": seat0_link["token"], "displayName": "Intruder", "forceTakeover": True},
        timeout=5,
    )
    assert ok.status_code == 200, f"forced takeover failed: {ok.status_code} {ok.text}"
    session2 = ok.json()["sessionToken"]
    assert session2 and session2 != session1, "new session token must differ"

    # 5) The original WS should receive a session_invalidated error frame.
    got_invalidated = False
    deadline = time.time() + 5
    while time.time() < deadline and not got_invalidated:
        try:
            raw = await asyncio.wait_for(ws1.recv(), timeout=deadline - time.time())
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            break
        m = json.loads(raw)
        if m.get("type") == "error" and m.get("code") == "session_invalidated":
            got_invalidated = True
            break
    assert got_invalidated, "original WS never received session_invalidated"

    try: await ws1.close()
    except Exception: pass

    print("OK: takeover requires confirm, succeeds with forceTakeover, "
          "and original session is invalidated.")


def main():
    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["MAHJONG_WEB_DIR"] = WEB_DIR
    # Long takeover threshold so the AI-revert worker doesn't interfere.
    env["MAHJONG_IDLE_ACT_MS"] = "60000"
    env["MAHJONG_IDLE_TAKEOVER_MS"] = "300000"
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
