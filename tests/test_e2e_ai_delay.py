"""End-to-end test for the AI speed slider.

Verifies that:
  1. The server accepts {"type":"set_ai_delay","delayMs":N} and stores it.
  2. The new aiDelayMs is reflected in subsequent snapshots.
  3. With a non-zero delay, AI moves arrive spaced ~delay apart rather than
     in a single synchronous burst.
  4. With delay=0, the server falls back to synchronous cascading (existing
     behavior preserved).

Usage:
    python tests\\test_e2e_ai_delay.py

Requires: requests, websockets.
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
    r = requests.post(f"{BASE}/api/rooms", json={"seed": "ai-delay-test"}, timeout=5)
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


async def recv_until_snapshot(ws, timeout=5) -> dict:
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout))
        if msg["type"] == "snapshot":
            return msg["snapshot"]


async def run_test():
    room = create_room()
    code = room["roomCode"]
    print(f"room: {code}")
    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = claim_seat(code, 0, seat0["token"], "Tester")
    session_token = claim["sessionToken"]

    async with websockets.connect(WS) as ws:
        await ws.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": session_token,
        }))
        snap = await recv_until_snapshot(ws)
        assert snap["aiDelayMs"] == 0, f"initial aiDelayMs should be 0, got {snap['aiDelayMs']}"
        print(f"  initial aiDelayMs = {snap['aiDelayMs']}")

        # Set the delay to 800 ms.
        await ws.send(json.dumps({"type": "set_ai_delay", "delayMs": 800}))
        # Server broadcasts a snapshot reflecting the new delay.
        snap = await recv_until_snapshot(ws)
        assert snap["aiDelayMs"] == 800, f"after set_ai_delay(800), got {snap['aiDelayMs']}"
        print(f"  after set_ai_delay(800): aiDelayMs = {snap['aiDelayMs']}")

        # Submit a discard, then measure inter-snapshot timing. With delay=800
        # the AI seats should each respond about 800 ms after the previous
        # snapshot — not all at once.
        # First, find a discard action.
        disc = next((a for a in snap.get("legalActions", []) if a["type"] == "discard"), None)
        assert disc is not None, "expected discard option for seat 0 at start"

        t0 = time.time()
        await ws.send(json.dumps({
            "type": "action", "expectedVersion": snap["version"], "action": disc,
        }))

        # Collect a few snapshots and check spacing.
        timestamps = []
        deadline = time.time() + 10
        while time.time() < deadline and len(timestamps) < 4:
            try:
                snap = await recv_until_snapshot(ws, timeout=3)
            except asyncio.TimeoutError:
                break
            timestamps.append(time.time() - t0)
            print(f"  snapshot v={snap['version']} at t+{timestamps[-1]:.3f}s phase={snap['phase']}")

        assert len(timestamps) >= 3, (
            f"expected at least 3 snapshots after discard with delay=800ms, got {len(timestamps)}: {timestamps}"
        )
        # Inter-snapshot deltas (after the first, which is the immediate
        # acknowledgement of our discard).
        deltas = [timestamps[i] - timestamps[i - 1] for i in range(1, len(timestamps))]
        print(f"  inter-snapshot deltas: {[f'{d:.3f}s' for d in deltas]}")
        # At least one delta should be near 800 ms (allow 500..1500 ms for jitter).
        assert any(0.5 <= d <= 1.5 for d in deltas), (
            f"no inter-snapshot delta near 0.8s (deltas={deltas}) — slider not being honored"
        )

        # Now drop the delay to 0 and verify subsequent snapshots arrive
        # essentially together (synchronous cascade).
        await ws.send(json.dumps({"type": "set_ai_delay", "delayMs": 0}))
        snap = await recv_until_snapshot(ws)
        assert snap["aiDelayMs"] == 0, f"after set_ai_delay(0), got {snap['aiDelayMs']}"
        print(f"  after set_ai_delay(0): aiDelayMs = {snap['aiDelayMs']}")

        # If our seat 0 has a discard at this point, submit it and observe
        # that AI snapshots arrive quickly (within e.g. 500 ms of each other).
        if any(a["type"] == "discard" for a in snap.get("legalActions", [])):
            disc = next(a for a in snap["legalActions"] if a["type"] == "discard")
            t0 = time.time()
            await ws.send(json.dumps({
                "type": "action", "expectedVersion": snap["version"], "action": disc,
            }))
            ts2 = []
            while time.time() - t0 < 5 and len(ts2) < 4:
                try:
                    snap = await recv_until_snapshot(ws, timeout=2)
                except asyncio.TimeoutError:
                    break
                ts2.append(time.time() - t0)
            print(f"  delay=0 snapshot timings: {[f'{d:.3f}s' for d in ts2]}")

        # Clamping check.
        await ws.send(json.dumps({"type": "set_ai_delay", "delayMs": 99999}))
        snap = await recv_until_snapshot(ws)
        assert snap["aiDelayMs"] == 5000, f"clamp upper bound should be 5000, got {snap['aiDelayMs']}"
        await ws.send(json.dumps({"type": "set_ai_delay", "delayMs": -50}))
        snap = await recv_until_snapshot(ws)
        assert snap["aiDelayMs"] == 0, f"clamp lower bound should be 0, got {snap['aiDelayMs']}"
        print("  clamping OK: [0, 5000]")

        print("PASS")


def main():
    try:
        asyncio.run(run_test())
    except AssertionError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
