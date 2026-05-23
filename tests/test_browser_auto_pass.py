"""Browser-driven test for the Auto-Pass checkbox.

Reproduces the user-reported bug: "checking auto-pass does not actually pass."

Strategy:
  1. Launch a Chromium page against the live server.
  2. Create a room and join as seat 0.
  3. Drive AI players via WS until the BROWSER's snapshot shows an optional-claim
     window for seat 0 (chow/pong/kong + pass, no win).
  4. Check the autoPass checkbox programmatically.
  5. Verify the snapshot version in the browser changes within 2 seconds.

Requires: playwright + requests + websockets.
"""
from __future__ import annotations

import asyncio
import json
import sys
import time

import requests
import websockets
from playwright.sync_api import sync_playwright

BASE = "http://localhost:18080"
WS = "ws://localhost:18080/ws"


def create_room() -> dict:
    r = requests.post(f"{BASE}/api/rooms", json={"seed": "browser-auto-pass-test"}, timeout=5)
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


async def drive_until_claim_window(code: str, session_token: str, page_eval, page_wait):
    """Drive the game via a side-channel WS connection while watching the
    browser's perceived snapshot. Stops when the BROWSER sees a snapshot with
    an optional-claim window for seat 0.
    """
    # We use a separate WS to keep ticking the game; the browser observes
    # snapshot broadcasts independently.
    # IMPORTANT: We do NOT submit actions for seat 0 via this side channel —
    # the browser owns seat 0. We only consume snapshots to know when to give
    # up if no opportunity arises.
    async with websockets.connect(WS) as ws:
        await ws.send(json.dumps({
            "type": "hello", "roomCode": code,
            "seatIndex": 0, "sessionToken": session_token,
        }))
        for _ in range(60):
            try:
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
            except asyncio.TimeoutError:
                break
            if msg["type"] != "snapshot":
                continue
            snap = msg["snapshot"]
            # If snapshot has chow/pong/kong + pass and no win, we're in the
            # target state. But we also need the BROWSER to have processed it.
            actions = snap.get("legalActions", [])
            types = [a["type"] for a in actions]
            if "pass" in types and any(t in ("chow", "pong", "kong") for t in types) and "win" not in types:
                # Wait until the browser snapshot version >= server version.
                target_version = snap["version"]
                # poll the browser
                deadline = time.time() + 5
                while time.time() < deadline:
                    bv = page_eval(
                        "() => window.state && window.state.snapshot ? window.state.snapshot.version : -1"
                    )
                    if bv is not None and bv >= target_version:
                        return target_version, types
                    page_wait(100)
                raise RuntimeError(
                    f"browser did not catch up to server version {target_version} (last seen {bv})"
                )
            # Game not in target state; if it's our discard turn, submit a discard
            # to keep things moving. We must drive the game from this WS because
            # the browser will *also* try to submit (auto-draw). We avoid double
            # submission by only submitting when the browser has NOT auto-drawn
            # yet (we let auto-draw handle draws).
            # Strategy: if there are non-pass / non-claim actions (i.e. we're
            # the active player), let the browser do it; otherwise just keep
            # listening.
        raise RuntimeError("no optional-claim window for seat 0 seen in 60 messages")


def main():
    room = create_room()
    code = room["roomCode"]
    print(f"room: {code}")

    seat0 = next(l for l in room["claimLinks"] if l["seatIndex"] == 0)
    claim = claim_seat(code, 0, seat0["token"], "Test Human")
    session_token = claim["sessionToken"]

    url = f"{BASE}/?room={code}&seat=0&token={seat0['token']}"
    print(f"opening: {url}")

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        ctx = browser.new_context(viewport={"width": 1280, "height": 900})
        page = ctx.new_page()
        page.on("console", lambda msg: print(f"  [browser:{msg.type}] {msg.text}"))
        page.goto(url)
        page.fill("#joinDisplayName", "Test Human")
        page.evaluate("() => { const d=document.getElementById('joinAdvanced'); if (d) d.open = true; }")
        page.click("#joinRoomBtn")
        page.wait_for_selector("#table:not(.hidden)", timeout=10000)
        page.wait_for_function(
            "() => window.state && window.state.snapshot && window.state.snapshot.players && window.state.snapshot.players.length === 4",
            timeout=10000,
        )

        # Drive until browser shows an optional-claim window for seat 0.
        # Side-channel: open another WS as the *same* seat — that's not
        # allowed by our server (only one session token validates), so
        # instead we connect as spectator and just observe.
        # Simpler approach: don't drive at all. The server's AI cascade after
        # our auto-draw will eventually produce a chow opportunity for us.
        # Just wait for the browser's snapshot to show one.
        # Install a tiny "auto-discard" helper that picks the first discard
        # whenever the browser sees discard options. Without this, the game
        # would stall on seat 0's discard step (the only action with no auto
        # behavior). This is purely test scaffolding.
        page.evaluate(
            """
            () => {
              window.__testAutoDiscard = setInterval(() => {
                const s = window.state.snapshot;
                if (!s) return;
                if (s.phase !== 'awaiting_discard') return;
                if (s.currentTurn !== window.state.seatIndex) return;
                const actions = s.legalActions || [];
                if (actions.length === 0) return;
                if (!actions.every(a => a.type === 'discard')) return;
                const a = actions[0];
                console.log('auto-discard:', a.tileId, 'v=' + s.version);
                window.state.ws.send(JSON.stringify({
                  type: 'action',
                  expectedVersion: s.version,
                  action: a,
                }));
              }, 250);
            }
            """
        )

        # PRE-CHECK the auto-pass toggle so it's on from the start. This tests
        # the path where auto-pass should fire from onSnapshot itself.
        page.click("#autoAiToggle")
        ap_pre = page.evaluate("() => ({ checked: document.getElementById('autoAiToggle').checked, state: window.state.autoPass })")
        print(f"  pre-toggled autoPass: {ap_pre}")
        assert ap_pre["checked"] and ap_pre["state"], "pre-toggle failed"

        print("Verifying auto-pass behaviour on claim windows (120s)...")
        deadline = time.time() + 120
        passed_only_windows = 0       # pass-only windows that auto-passed
        call_windows_seen = 0         # chow/pong/kong windows seen
        call_windows_interrupted = 0  # call windows where auto-pass correctly did NOT fire
        passed_versions = []
        last_print = 0
        last_v = -1

        def snapshot():
            return page.evaluate(
                "() => ({version: window.state.snapshot ? window.state.snapshot.version : -1, "
                "phase: window.state.snapshot ? window.state.snapshot.phase : null, "
                "turn: window.state.snapshot ? window.state.snapshot.currentTurn : -1, "
                "types: window.state.snapshot ? (window.state.snapshot.legalActions || []).map(a => a.type) : [], "
                "actions: window.state.snapshot ? (window.state.snapshot.legalActions || []) : []})"
            )

        def manual_pass(s):
            """Send a pass for the current snapshot via the browser's ws."""
            pass_action = next((a for a in s["actions"] if a["type"] == "pass"), None)
            if pass_action is None:
                return False
            page.evaluate(
                "(payload) => window.state.ws.send(JSON.stringify(payload))",
                {"type": "action", "expectedVersion": s["version"], "action": pass_action},
            )
            return True

        CALL_TYPES = {"win", "chow", "pong", "kong"}

        while time.time() < deadline:
            s = snapshot()
            v = s["version"]
            phase = s["phase"]
            types = s["types"]
            if v != last_v or time.time() - last_print > 5:
                print(f"  v={v} phase={phase} turn={s['turn']} types={types}")
                last_print = time.time()
                last_v = v

            has_call = any(t in CALL_TYPES for t in types)
            has_pass = "pass" in types

            if phase == "awaiting_claims" and has_pass and has_call:
                # CALL WINDOW: auto-pass MUST NOT fire. Wait 1.5s, verify
                # the version is unchanged.
                start_v = v
                interrupted = True
                start = time.time()
                while time.time() - start < 1.5:
                    s2 = snapshot()
                    if s2["version"] != start_v:
                        interrupted = False
                        break
                    page.wait_for_timeout(100)
                call_windows_seen += 1
                if interrupted:
                    call_windows_interrupted += 1
                    print(f"  >> call window v={start_v} types={types} -> auto-pass INTERRUPTED (correct)")
                    # Manually pass to keep the game advancing.
                    s_now = snapshot()
                    if s_now["phase"] == "awaiting_claims" and "pass" in s_now["types"]:
                        manual_pass(s_now)
                else:
                    print(f"FAIL: call window v={start_v} types={types} was auto-passed "
                          f"(should have waited for user)", file=sys.stderr)
                    sys.exit(1)
                continue

            if phase == "awaiting_claims" and has_pass and not has_call:
                # PASS-ONLY WINDOW: auto-pass MUST fire within 3s.
                start_v = v
                start = time.time()
                advanced = False
                while time.time() - start < 3.0:
                    if snapshot()["version"] > start_v:
                        advanced = True
                        break
                    page.wait_for_timeout(100)
                if not advanced:
                    diag = page.evaluate(
                        "() => ({lastAutoActVersion: window.state.lastAutoActVersion, "
                        "autoPass: window.state.autoPass, snapshotVersion: window.state.snapshot.version, "
                        "types: (window.state.snapshot.legalActions||[]).map(a => a.type), "
                        "wsState: window.state.ws ? window.state.ws.readyState : -1})"
                    )
                    print(f"FAIL: pass-only window at v={start_v} did not auto-pass "
                          f"within 3s (types={types})", file=sys.stderr)
                    print(f"  diagnostics: {diag}", file=sys.stderr)
                    sys.exit(1)
                passed_only_windows += 1
                passed_versions.append(start_v)
                print(f"  >> pass-only window v={start_v} -> auto-passed (correct)")
                continue

            if phase == "finished":
                break
            page.wait_for_timeout(150)

        print(f"Summary: pass-only auto-passed={passed_only_windows} (versions={passed_versions}), "
              f"call windows interrupted={call_windows_interrupted}/{call_windows_seen}")

        if passed_only_windows == 0 and call_windows_seen == 0:
            print("FAIL: did not observe any claim windows for seat 0 in 120s",
                  file=sys.stderr)
            sys.exit(1)
        if call_windows_seen > 0 and call_windows_interrupted != call_windows_seen:
            print(f"FAIL: {call_windows_seen - call_windows_interrupted} call window(s) "
                  f"were incorrectly auto-passed", file=sys.stderr)
            sys.exit(1)

        browser.close()
        print("PASS")


if __name__ == "__main__":
    main()
