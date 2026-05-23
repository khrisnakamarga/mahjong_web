"""End-to-end test for the 3 improvements landed in this session:

  1. **Chicken Hand toggle (minFan)**: snapshot.minFan defaults to 3; setting
     it via `{type:'set_min_fan',value:0}` is persisted and broadcast across
     all clients in the room.
  2. **(you) label scoping**: client-side correctness verified separately via
     Playwright (see test_e2e_you_label.py). Here we verify the server-side
     contract: snapshot.players[*].controller exposes the seat's controller
     so the client can decide locally.
  3. **Claim precedence gate**: verified via the C++ unit test
     `testClaimPrecedenceGate` in tests/test_core.cpp. This file's job is to
     confirm the WS surface accepts and rejects appropriately for Chicken
     Hand.

Usage:
    python tests\\test_e2e_chicken_hand.py

Requires: requests, websockets, and the C++ web server running on
localhost:18080.
"""
from __future__ import annotations

import asyncio
import json
import time

import requests
import websockets

BASE = "http://localhost:18080"
WS = "ws://localhost:18080/ws"


def create_room() -> dict:
    r = requests.post(f"{BASE}/api/rooms", json={"seed": f"chicken-{time.time()}"}, timeout=5)
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


async def recv_snapshot(ws, timeout=5) -> dict:
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout))
        if msg["type"] == "snapshot":
            return msg["snapshot"]
        if msg["type"] == "error":
            raise RuntimeError(f"server error: {msg}")


async def run():
    room = create_room()
    code = room["roomCode"]
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim0 = claim_seat(code, 0, seat0["token"], "Alice")
    print(f"room={code} (seat 0 claimed)")

    async with websockets.connect(WS) as ws_a:
        await ws_a.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": claim0["sessionToken"],
        }))
        snap_a = await recv_snapshot(ws_a)

        # 1) Default minFan is 3 (standard Hong Kong Mahjong).
        assert snap_a.get("minFan") == 3, \
            f"FAIL: fresh room minFan should be 3, got {snap_a.get('minFan')!r}"
        print(f"  initial: minFan={snap_a['minFan']}")

        # 2) Server-side contract: players each expose `controller` so the
        #    client can decide locally that only the viewer's plaque shows
        #    "(you)". We verify both human/ai controllers appear.
        controllers = {p.get("controller") for p in snap_a.get("players", [])}
        assert "human" in controllers, f"FAIL: seat 0 should be human, got {controllers}"
        assert "ai" in controllers, f"FAIL: at least one seat should be ai, got {controllers}"
        # Verify exactly one seat is human in this test (seat 0 only claimed).
        humans = [p for p in snap_a["players"] if p.get("controller") == "human"]
        assert len(humans) == 1 and humans[0]["seatIndex"] == 0, (
            f"FAIL: only seat 0 should be human, got {[(p['seatIndex'], p['controller']) for p in snap_a['players']]}"
        )
        print(f"  controllers: {[p.get('controller') for p in snap_a['players']]}")

        # 3) Toggle Chicken Hand ON via set_min_fan value=0.
        await ws_a.send(json.dumps({"type": "set_min_fan", "value": 0}))
        snap_a = await recv_snapshot(ws_a)
        assert snap_a["minFan"] == 0, \
            f"FAIL: after set_min_fan(0) snapshot.minFan should be 0, got {snap_a['minFan']!r}"
        print(f"  after set_min_fan(0): minFan={snap_a['minFan']}")

        # 4) Room-scoped: a second connection should see the same minFan.
        async with websockets.connect(WS) as ws_b:
            await ws_b.send(json.dumps({"type": "hello", "roomCode": code}))
            snap_b = await recv_snapshot(ws_b)
            assert snap_b["minFan"] == 0, (
                f"FAIL: spectator B should observe room-scoped minFan=0, "
                f"got {snap_b['minFan']!r}"
            )
            print(f"  B (spectator): sees minFan={snap_b['minFan']} (room-scoped OK)")

            # 5) Flip back to standard 3 via B.
            await ws_b.send(json.dumps({"type": "set_min_fan", "value": 3}))
            snap_b = await recv_snapshot(ws_b)
            assert snap_b["minFan"] == 3
            snap_a = await recv_snapshot(ws_a)
            assert snap_a["minFan"] == 3, (
                f"FAIL: connection A should receive minFan=3 broadcast from B, "
                f"got {snap_a['minFan']!r}"
            )
            print(f"  A received B's flip: minFan={snap_a['minFan']}")

        # 6) Clamping: setting absurdly high should be capped at 13.
        await ws_a.send(json.dumps({"type": "set_min_fan", "value": 999}))
        snap_a = await recv_snapshot(ws_a)
        assert snap_a["minFan"] == 13, \
            f"FAIL: minFan=999 should clamp to 13, got {snap_a['minFan']!r}"
        # Negative -> 0.
        await ws_a.send(json.dumps({"type": "set_min_fan", "value": -5}))
        snap_a = await recv_snapshot(ws_a)
        assert snap_a["minFan"] == 0, \
            f"FAIL: minFan=-5 should clamp to 0, got {snap_a['minFan']!r}"
        print(f"  clamping OK: 999->13, -5->0")

    print("\nPASS")


if __name__ == "__main__":
    asyncio.run(run())
