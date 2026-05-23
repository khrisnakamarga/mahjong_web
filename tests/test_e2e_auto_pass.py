"""End-to-end test for auto-pass behavior.

Drives the C++ web server until a chow/pong opportunity reaches seat 0 (the
human seat), then submits a `pass` action via WS and verifies the server
accepts it (version increments). This catches:

  * server illegally rejecting a valid pass action,
  * stale `pendingClaimPasses` blocking the human seat from passing again,
  * legalActions JSON round-trip mismatches (equality must hold byte-for-byte).

Usage:
    python tests\\test_e2e_auto_pass.py

Requires: requests, websockets, and a running server on localhost:18080.
"""
from __future__ import annotations

import asyncio
import json
import sys
import time
from typing import Optional

import requests
import websockets

BASE = "http://localhost:18080"
WS = "ws://localhost:18080/ws"


def create_room() -> dict:
    r = requests.post(f"{BASE}/api/rooms", json={"seed": "auto-pass-test"}, timeout=5)
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


async def run_test():
    room = create_room()
    code = room["roomCode"]
    print(f"room: {code}")

    # Only claim seat 0; let the other three stay AI.
    seat0_link = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = claim_seat(code, 0, seat0_link["token"], "Test Human")
    session_token = claim["sessionToken"]

    found_claim_window = False
    claim_window_actions = []

    async with websockets.connect(WS) as ws:
        await ws.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": session_token,
        }))
        # Receive welcome + initial snapshot
        snapshot = await recv_snapshot(ws)

        # Play up to N turns. On each, if we're offered a claim option (chow/
        # pong/kong + pass), record it and send pass. If we have draw, draw.
        # If we have discard, discard the first one. Stop after seat 0 sees
        # at least one optional-claim window AND successfully passes it.
        for turn in range(60):
            actions = snapshot.get("legalActions", [])
            types = [a["type"] for a in actions]
            print(f"  turn {turn}: phase={snapshot['phase']} actions={types}")
            has_claim_options = any(t in ("chow", "pong", "kong") for t in types)
            pass_action = next((a for a in actions if a["type"] == "pass"), None)
            win_action = next((a for a in actions if a["type"] == "win"), None)

            if has_claim_options and pass_action and not win_action:
                # The auto-pass scenario: snapshot offers optional claims + pass.
                # Verify pass works.
                pre_version = snapshot["version"]
                claim_window_actions = list(types)
                print(f"  >> auto-pass scenario found at version {pre_version}: {types}")
                snapshot = await submit(ws, snapshot, pass_action)
                assert snapshot["version"] > pre_version, (
                    f"server did not accept pass (version unchanged: {snapshot['version']})"
                )
                print(f"  >> pass accepted: version {pre_version} -> {snapshot['version']}")
                found_claim_window = True
                # Continue playing a bit to make sure the game advances.
                continue

            # Otherwise, play normally to keep the game moving.
            draw = next((a for a in actions if a["type"] == "draw"), None)
            disc = next((a for a in actions if a["type"] == "discard"), None)
            if draw:
                snapshot = await submit(ws, snapshot, draw)
                continue
            if pass_action:
                # Pass-only window (e.g., someone else discarded and we have no claim).
                snapshot = await submit(ws, snapshot, pass_action)
                continue
            if disc:
                snapshot = await submit(ws, snapshot, disc)
                continue
            # Nothing to do; wait for next snapshot.
            try:
                snapshot = await recv_snapshot(ws, timeout=3)
            except asyncio.TimeoutError:
                break

            if found_claim_window:
                # Already verified; just keep playing a few more turns.
                if turn > 50:
                    break

        assert found_claim_window, (
            "Did not encounter an optional-claim window for seat 0 in 60 turns. "
            "Try a different seed or increase the turn count."
        )
        print("OK: at least one optional-claim window seen and passed successfully.")
        print(f"     last seen actions: {claim_window_actions}")


def main():
    try:
        asyncio.run(run_test())
    except AssertionError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
