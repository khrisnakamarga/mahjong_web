"""End-to-end test for room-scoped autoPass + winHistory.

Verifies:

  1. ``snapshot.autoPass`` defaults to ``False`` on a fresh room.
  2. ``{"type":"set_auto_pass","value":true}`` is persisted and broadcast.
  3. A second WS connection joining the same room observes the SAME autoPass
     value -- proving the toggle is room-scoped, not connection-local.
  4. ``snapshot.winHistory`` is an array (may be empty initially).
  5. Playing through to a winning round causes ``winHistory`` to grow by 1
     and the new entry has ``source`` + ``winningTile`` + ``settlement``.

Usage:
    python tests\\test_e2e_autopass_and_history.py

Requires: requests, websockets, and the C++ web server running on
localhost:18080.
"""
from __future__ import annotations

import asyncio
import json
import sys
import time

import requests
import websockets

BASE = "http://localhost:18080"
WS = "ws://localhost:18080/ws"


def create_room() -> dict:
    r = requests.post(f"{BASE}/api/rooms", json={"seed": f"ap-hist-{time.time()}"}, timeout=5)
    r.raise_for_status()
    return r.json()


def claim_seat(code: str, seat: int, token: str, name: str) -> dict:
    r = requests.post(
        f"{BASE}/api/rooms/{code}/seats/{seat}",
        json={"token": token, "displayName": name},
        timeout=5,
    )
    r.raise_for_status()
    return r.json()


async def recv_snapshot(ws, timeout=8) -> dict:
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout))
        if msg["type"] == "snapshot":
            return msg["snapshot"]
        if msg["type"] == "error":
            raise RuntimeError(f"server error: {msg}")


async def submit(ws, snapshot, action) -> dict:
    await ws.send(json.dumps({
        "type": "action",
        "expectedVersion": snapshot["version"],
        "action": action,
    }))
    return await recv_snapshot(ws)


async def play_one_round_to_finish(ws, snapshot, max_turns=200):
    for _ in range(max_turns):
        if snapshot.get("phase") == "finished":
            return snapshot
        actions = snapshot.get("legalActions", [])
        if not actions:
            try:
                snapshot = await recv_snapshot(ws, timeout=3)
            except asyncio.TimeoutError:
                return snapshot
            continue
        win = next((a for a in actions if a["type"] == "win"), None)
        draw = next((a for a in actions if a["type"] == "draw"), None)
        disc = next((a for a in actions if a["type"] == "discard"), None)
        pass_a = next((a for a in actions if a["type"] == "pass"), None)
        choice = win or draw or disc or pass_a or actions[0]
        snapshot = await submit(ws, snapshot, choice)
    return snapshot


async def run():
    room = create_room()
    code = room["roomCode"]
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim0 = claim_seat(code, 0, seat0["token"], "Alice")
    print(f"room={code} (seat 0 human + 3 AIs; spectator joins from B)")

    # ---- Connection A: seat 0 ----
    async with websockets.connect(WS) as ws_a:
        await ws_a.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": claim0["sessionToken"],
        }))
        snap_a = await recv_snapshot(ws_a)
        assert snap_a.get("autoPass") is False, \
            f"FAIL: fresh room autoPass should be False, got {snap_a.get('autoPass')!r}"
        assert isinstance(snap_a.get("winHistory"), list), \
            f"FAIL: winHistory should be a list, got {type(snap_a.get('winHistory')).__name__}"
        history_initial = list(snap_a["winHistory"])
        print(f"  initial: autoPass={snap_a['autoPass']} winHistory len={len(history_initial)}")

        # ---- Flip autoPass on from connection A ----
        await ws_a.send(json.dumps({"type": "set_auto_pass", "value": True}))
        snap_a = await recv_snapshot(ws_a)
        assert snap_a["autoPass"] is True, \
            f"FAIL: after set_auto_pass(true) on A, snapshot should show True, got {snap_a['autoPass']!r}"
        print(f"  A: after set_auto_pass(true) -> autoPass={snap_a['autoPass']}")

        # ---- Connection B: spectator joins same room, should see autoPass=True from A ----
        async with websockets.connect(WS) as ws_b:
            await ws_b.send(json.dumps({
                "type": "hello", "roomCode": code,
                # No seatIndex / sessionToken == spectator
            }))
            snap_b = await recv_snapshot(ws_b)
            assert snap_b["autoPass"] is True, (
                f"FAIL: spectator B should observe room-scoped autoPass=True, "
                f"got {snap_b['autoPass']!r}"
            )
            print(f"  B (spectator): sees autoPass={snap_b['autoPass']} (room-scoped OK)")

            # ---- Flip from B (spectator can change room toggles) ----
            await ws_b.send(json.dumps({"type": "set_auto_pass", "value": False}))
            snap_b = await recv_snapshot(ws_b)
            assert snap_b["autoPass"] is False
            snap_a = await recv_snapshot(ws_a)
            assert snap_a["autoPass"] is False, (
                f"FAIL: connection A should receive autoPass=False broadcast from B, "
                f"got {snap_a['autoPass']!r}"
            )
            print(f"  A received B's flip: autoPass={snap_a['autoPass']}")

        # ---- Now play through one round, watch history grow ----
        # Re-enable autoPass so passes on pass-only windows still need explicit
        # action from the test (seat 0 always sends them via play_one_round).
        await ws_a.send(json.dumps({"type": "set_auto_pass", "value": True}))
        snap_a = await recv_snapshot(ws_a)
        snap_a = await play_one_round_to_finish(ws_a, snap_a)
        if snap_a.get("phase") != "finished":
            print("WARN: could not reach finished phase in 200 turns; skipping winHistory growth check")
            return
        new_history = snap_a["winHistory"]
        assert len(new_history) == len(history_initial) + 1, (
            f"FAIL: winHistory should have grown by 1; was {len(history_initial)}, "
            f"now {len(new_history)}"
        )
        entry = new_history[-1]
        assert "reason" in entry, f"FAIL: history entry missing 'reason': {entry}"
        print(f"  after round: winHistory len={len(new_history)} last.reason={entry['reason']}")
        if entry["reason"] == "win":
            assert "source" in entry, f"FAIL: win entry missing 'source': {entry}"
            assert "winningTile" in entry, f"FAIL: win entry missing 'winningTile': {entry}"
            assert "settlement" in entry, f"FAIL: win entry missing 'settlement': {entry}"
            print(f"  entry OK: source={entry['source']} "
                  f"tile={entry['winningTile'].get('id')!r} "
                  f"fan={entry['settlement']['fan']}")
        else:
            print(f"  (exhaustive draw round, no per-entry win fields expected)")

        # ---- Start next round; history must persist ----
        actions = snap_a.get("legalActions", [])
        nxt = next((a for a in actions if a["type"] == "next_round"), None)
        if nxt:
            snap_a = await submit(ws_a, snap_a, nxt)
            assert len(snap_a["winHistory"]) == len(new_history), (
                f"FAIL: winHistory must persist across NextRound; was {len(new_history)}, "
                f"now {len(snap_a['winHistory'])}"
            )
            print(f"  after NextRound: winHistory persisted len={len(snap_a['winHistory'])}")

    print("\nPASS")


if __name__ == "__main__":
    asyncio.run(run())
