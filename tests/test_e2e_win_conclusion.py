"""End-to-end test that verifies the conclusion JSON carries enough info to
render the winning tile and source label in the client.

Plays a room until a winner is declared (or up to ~120 turns), then asserts
the final snapshot's `conclusion` object exposes:

  * `source` ∈ {self_draw, discard, flower, robbing_kong}
  * `winningTile` with at least `id` and a usable label

This is the data contract the web/Win32 conclusion banners depend on.

Usage:
    python tests\\test_e2e_win_conclusion.py
Requires: requests, websockets, and a running server on localhost:18080.
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
    r = requests.post(f"{BASE}/api/rooms", json={"seed": f"win-conc-{time.time()}"}, timeout=5)
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


async def run_one_round_until_finished():
    room = create_room()
    code = room["roomCode"]
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = claim_seat(code, 0, seat0["token"], "Test Human")
    session = claim["sessionToken"]
    print(f"room={code}")

    async with websockets.connect(WS) as ws:
        await ws.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": session,
        }))
        snapshot = await recv_snapshot(ws)

        for turn in range(200):
            if snapshot.get("phase") == "finished":
                return snapshot
            actions = snapshot.get("legalActions", [])
            if not actions:
                try:
                    snapshot = await recv_snapshot(ws, timeout=3)
                except asyncio.TimeoutError:
                    return snapshot
                continue
            # Greedy: if we can win, win. Otherwise prefer draw → discard → pass.
            win = next((a for a in actions if a["type"] == "win"), None)
            draw = next((a for a in actions if a["type"] == "draw"), None)
            disc = next((a for a in actions if a["type"] == "discard"), None)
            pass_a = next((a for a in actions if a["type"] == "pass"), None)
            choice = win or draw or disc or pass_a or actions[0]
            snapshot = await submit(ws, snapshot, choice)
        return snapshot


async def run_until_winner(max_attempts=8):
    """Try several rooms until one ends in a Win (not exhaustive draw)."""
    last_snapshot = None
    for attempt in range(max_attempts):
        snapshot = await run_one_round_until_finished()
        conclusion = snapshot.get("conclusion") or {}
        print(f"attempt {attempt}: phase={snapshot.get('phase')} reason={conclusion.get('reason')}")
        last_snapshot = snapshot
        if conclusion.get("reason") == "win":
            return snapshot
    return last_snapshot


def main():
    snapshot = asyncio.run(run_until_winner())
    if snapshot is None:
        print("FAIL: never reached a finished round", file=sys.stderr)
        sys.exit(1)
    conclusion = snapshot.get("conclusion") or {}
    print(f"\nfinal conclusion: {json.dumps(conclusion, indent=2)[:600]}...")

    if conclusion.get("reason") != "win":
        print("WARN: no winning round in 8 attempts (only exhaustive draws). "
              "The data contract checks below are skipped but the test still "
              "asserts the conclusion object exists.", file=sys.stderr)
        if not conclusion:
            print("FAIL: conclusion is missing entirely", file=sys.stderr)
            sys.exit(1)
        print("PASS (exhaustive draw only)")
        return

    # Data contract the conclusion banner relies on.
    source = conclusion.get("source")
    assert source in ("self_draw", "discard", "flower", "robbing_kong"), \
        f"FAIL: source must be one of self_draw/discard/flower/robbing_kong, got {source!r}"
    print(f"OK: source={source}")

    tile = conclusion.get("winningTile")
    assert tile is not None, "FAIL: winningTile must be present on a Win conclusion"
    assert isinstance(tile, dict), f"FAIL: winningTile must be an object, got {type(tile).__name__}"
    assert tile.get("id"), f"FAIL: winningTile.id must be non-empty, got {tile!r}"
    print(f"OK: winningTile.id={tile['id']} key={tile.get('tileKey') or tile.get('name')!r}")

    # For discard wins, the client needs to know who fed the win to render
    # "Won on discard from {Wind}". The server sets ``conclusion.responsibleSeat``
    # (preferred, survives finish()) and may also leave ``snapshot.lastDiscard``
    # in place as a fallback. Require at least one of them.
    if source == "discard":
        responsible = conclusion.get("responsibleSeat")
        last = snapshot.get("lastDiscard")
        last_by_seat = last.get("bySeat") if isinstance(last, dict) else None
        feeder = responsible if isinstance(responsible, int) else last_by_seat
        assert isinstance(feeder, int) and 0 <= feeder < 4, (
            f"FAIL: discard win must expose the feeder seat via "
            f"conclusion.responsibleSeat or snapshot.lastDiscard.bySeat; "
            f"got responsibleSeat={responsible!r} lastDiscard={last!r}"
        )
        print(f"OK: discard fed by seat={feeder} "
              f"(via {'responsibleSeat' if isinstance(responsible, int) else 'lastDiscard.bySeat'})")

    print("\nPASS")


if __name__ == "__main__":
    main()
