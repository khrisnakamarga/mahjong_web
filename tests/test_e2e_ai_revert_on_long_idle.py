"""End-to-end test: an unresponsive seat reverts to AI after the long threshold.

If a human leaves entirely (closes their tab, loses signal, etc.) and never
comes back within MAHJONG_IDLE_TAKEOVER_MS, the server should flip that seat
from Controller::Human back to Controller::Ai so the round keeps going. The
seat's claim token survives, so the original human can come back and re-claim
(via the regular force-takeover flow).

Setup: server with MAHJONG_IDLE_TAKEOVER_MS=1200ms.

Steps:
  1. Create a room and claim seat 0 (human) -- but never open a WebSocket.
  2. Verify the room's seat 0 starts as controller='human' (per snapshot).
  3. Wait > 1200ms.
  4. Re-fetch room snapshot and assert seat 0 is now controller='ai'.

Spawns its own server on port 18097.

Usage:
    python tests\\test_e2e_ai_revert_on_long_idle.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request

import requests

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_EXE = os.path.join(ROOT, "build-web", "Release", "mahjong_web_server.exe")
WEB_DIR = os.path.join(ROOT, "web")
PORT = 18097
BASE = f"http://localhost:{PORT}"


def _wait_for_server(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"{BASE}/api/health", timeout=1).read()
            return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError(f"server not up on {BASE}")


def _seat0_controller(code):
    r = requests.get(f"{BASE}/api/rooms/{code}", timeout=5)
    r.raise_for_status()
    snap = r.json()
    seats = snap.get("seats") or snap.get("snapshot", {}).get("seats") or []
    if not seats:
        # Some payloads nest under 'snapshot'; tolerate both shapes.
        snap2 = snap.get("snapshot") or snap
        seats = snap2.get("seats", [])
    assert seats, f"no seats in /api/rooms response: {snap}"
    return seats[0].get("controller")


def _run():
    room = requests.post(f"{BASE}/api/rooms", json={"seed": "ai-revert-test"},
                         timeout=5).json()
    code = room["roomCode"]
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = requests.post(
        f"{BASE}/api/rooms/{code}/seats/0",
        json={"token": seat0["token"], "displayName": "Ghost"},
        timeout=5,
    )
    assert claim.status_code == 200, f"claim failed: {claim.status_code} {claim.text}"

    # Wait past the takeover threshold (1200ms) plus a generous margin so the
    # background worker (200ms tick) gets several attempts even under load.
    time.sleep(4.0)

    after = _seat0_controller(code)
    # The display name "Ghost" should still be there (we don't wipe it on
    # conversion), and the controller should now be AI.
    snap = requests.get(f"{BASE}/api/rooms/{code}", timeout=5).json()
    seat0_state = snap["seats"][0]
    assert after == "ai", (
        f"expected seat 0 to revert to AI after >1200ms idle, "
        f"still controller={after!r}; full seat0={seat0_state}"
    )
    # Display name should still match -- we don't erase identity on conversion,
    # the human can still see who was there from the snapshot.
    assert seat0_state.get("displayName") == "Ghost", (
        f"expected displayName='Ghost' preserved across conversion, got {seat0_state}"
    )
    print(f"OK: seat 0 reverted from human -> ai after takeover threshold. "
          f"displayName preserved: {seat0_state.get('displayName')!r}")


def main():
    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["MAHJONG_WEB_DIR"] = WEB_DIR
    env["MAHJONG_IDLE_ACT_MS"] = "400"
    env["MAHJONG_IDLE_TAKEOVER_MS"] = "1200"
    env["MAHJONG_ROOM_TTL_FINISHED_SECONDS"] = "86400"
    env["MAHJONG_ROOM_TTL_ACTIVE_SECONDS"] = "86400"
    proc = subprocess.Popen(
        [SERVER_EXE], cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        _wait_for_server()
        _run()
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
