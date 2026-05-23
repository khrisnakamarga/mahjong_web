"""End-to-end Playwright test for the `(you)` label scoping fix.

Before the fix, ANY human-controlled seat showed "(you)" next to its name,
which was confusing when 2-4 humans played together (every other player
appeared to also be "you"). After the fix, "(you)" must appear only on the
viewer's own plaque.

Strategy: monkey-patch `CanvasRenderingContext2D.fillText` in each tab to
record every drawn string, then assert exactly one drawn string contains
"(you)" and that string includes the viewer's display name.

Requires: playwright (chromium), and the C++ web server running on :18080
(launched by this script the same way test_e2e_mobile_ui.py does).
"""

import asyncio
import json
import os
import subprocess
import sys
import time
import urllib.request
from contextlib import contextmanager

# Force UTF-8 output so we can print the ◀ marker without cp1252 errors.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT      = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERVER    = os.path.join(ROOT, "build-web", "Release", "mahjong_web_server.exe")
WEB_DIR   = os.path.join(ROOT, "web")
PORT      = 18080
BASE      = f"http://localhost:{PORT}"


def _http(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE + path, data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def _wait_for_server():
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            urllib.request.urlopen(BASE + "/", timeout=1).read()
            return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError("server did not come up in time")


@contextmanager
def server_running():
    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["MAHJONG_WEB_DIR"] = WEB_DIR
    env["MAHJONG_ROOM_TTL_FINISHED_SEC"] = "86400"
    env["MAHJONG_ROOM_TTL_ACTIVE_SEC"]   = "86400"
    proc = subprocess.Popen(
        [SERVER], cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        _wait_for_server()
        yield
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            proc.kill()


PATCH_FILL_TEXT = """
if (typeof window.__textCalls === 'undefined') {
  window.__textCalls = [];
  const proto = CanvasRenderingContext2D.prototype;
  const orig = proto.fillText;
  proto.fillText = function(text, x, y, ...rest) {
    try { window.__textCalls.push(String(text)); } catch (e) {}
    return orig.call(this, text, x, y, ...rest);
  };
}
"""


async def _open_seat(ctx, room_code, seat_index, token, name):
    page = await ctx.new_page()
    # Install the fillText hook BEFORE the app loads so we catch the first frame.
    await page.add_init_script(PATCH_FILL_TEXT)
    url = f"{BASE}/?room={room_code}&seat={seat_index}&token={token}"
    await page.goto(url)
    await page.wait_for_load_state("networkidle")
    await page.fill("#joinDisplayName", name)
    # The "Join with token" button now lives inside a <details> advanced panel;
    # open it programmatically so Playwright can click the button.
    await page.evaluate(
        "() => { const d=document.getElementById('joinAdvanced');"
        " if (d) d.open = true; }"
    )
    await page.click("#joinRoomBtn")
    await page.wait_for_selector("#table:not(.hidden)", timeout=5000)
    await page.wait_for_function(
        "window.state && window.state.snapshot && window.state.snapshot.players",
        timeout=8000,
    )
    # Give the canvas a moment to render at least one full frame.
    await page.wait_for_timeout(500)
    # Force at least one redraw and verify the patch is live.
    diag = await page.evaluate(
        "() => ({ patched: typeof window.__textCalls !== 'undefined', "
        " count: (window.__textCalls||[]).length })"
    )
    if not diag["patched"]:
        raise RuntimeError(f"fillText hook never ran (diag={diag})")
    return page


async def _drawn_strings_with_you(page):
    """Return all canvas-drawn strings that contain '(you)'."""
    return await page.evaluate(
        "() => (window.__textCalls||[]).filter(t => t.includes('(you)'))"
    )


async def _all_drawn_strings(page):
    return await page.evaluate("() => Array.from(window.__textCalls||[])")


async def test_you_label_only_on_viewer_seat(p):
    print("=== test_you_label_only_on_viewer_seat ===")
    browser = await p.chromium.launch()
    contexts = [await browser.new_context() for _ in range(2)]

    room  = _http("POST", "/api/rooms", {})
    code  = room["roomCode"]
    links = room["claimLinks"]

    # Claim seat 0 as "Alice" and seat 1 as "Bob" — two real humans, two AIs.
    page0 = await _open_seat(contexts[0], code, links[0]["seatIndex"], links[0]["token"], "Alice")
    page1 = await _open_seat(contexts[1], code, links[1]["seatIndex"], links[1]["token"], "Bob")

    # The HTTP /api/rooms/.../seats/... claim endpoint does NOT push a fresh
    # snapshot to already-connected sockets, so Alice's tab still sees seat
    # 1 as "AI 2" at this point. Trigger any room-scoped setting change to
    # force the server to broadcast a fresh snapshot to every connection.
    await page0.evaluate(
        "() => { const ws = window.state && window.state.ws;"
        " if (ws && ws.readyState === 1) ws.send(JSON.stringify({type:'set_auto_pass', value:true})); }"
    )
    # Wait for the updated player names to land on both tabs.
    for pg, expected_names in ((page0, ("Alice", "Bob")), (page1, ("Alice", "Bob"))):
        await pg.wait_for_function(
            "(names) => { const s = window.state && window.state.snapshot;"
            " if (!s) return false;"
            " const got = (s.players||[]).map(p => p.displayName);"
            " return names.every(n => got.includes(n)); }",
            arg=list(expected_names),
            timeout=8000,
        )
    # Clear the recorded strings (they were dominated by initial-snapshot
    # draws when Bob hadn't joined yet) and force a fresh redraw on each tab.
    for pg in (page0, page1):
        await pg.evaluate("() => { if (window.__textCalls) window.__textCalls.length = 0; }")
        await pg.evaluate("() => { if (typeof draw === 'function') draw(); }")
    # Wait a beat for redraws to flush.
    await page0.wait_for_timeout(400)
    await page1.wait_for_timeout(400)

    # Diagnose what each tab actually sees.
    for label, pg in (("Alice", page0), ("Bob", page1)):
        diag = await pg.evaluate(
            "() => { const s = window.state && window.state.snapshot;"
            " if (!s) return null;"
            " return { players: (s.players||[]).map(p => ({seat: p.seatIndex, name: p.displayName, ctrl: p.controller}))}; }"
        )
        print(f"  {label} tab sees: {diag}")

    yous0 = await _drawn_strings_with_you(page0)
    yous1 = await _drawn_strings_with_you(page1)

    # Filter to plaque-style lines (must include the parenthetical and a name).
    plaque0 = [s for s in yous0 if "(you)" in s]
    plaque1 = [s for s in yous1 if "(you)" in s]

    print(f"  Alice tab: drew '(you)' on {plaque0}")
    print(f"  Bob   tab: drew '(you)' on {plaque1}")

    # Each tab must have AT LEAST one "(you)" draw (the viewer's plaque is
    # re-drawn on every frame, so we expect many, but at minimum one).
    assert plaque0, f"FAIL: Alice tab never drew '(you)'; samples: {await _all_drawn_strings(page0)}"
    assert plaque1, f"FAIL: Bob tab never drew '(you)'; samples: {await _all_drawn_strings(page1)}"

    # CRITICAL: every "(you)" string on the Alice tab must include "Alice",
    # not "Bob" (otherwise the regression has returned — Bob would also be
    # labeled "(you)" because he's human too).
    for s in plaque0:
        assert "Alice" in s, (
            f"FAIL: Alice tab drew '(you)' on a non-viewer plaque: {s!r} "
            f"(should only ever apply to Alice's own seat)"
        )
        assert "Bob" not in s, (
            f"FAIL: Alice tab drew '(you)' on Bob's plaque: {s!r} "
            f"(regression — '(you)' must scope to viewer only)"
        )
    for s in plaque1:
        assert "Bob" in s, (
            f"FAIL: Bob tab drew '(you)' on a non-viewer plaque: {s!r}"
        )
        assert "Alice" not in s, (
            f"FAIL: Bob tab drew '(you)' on Alice's plaque: {s!r}"
        )

    # Also verify Alice's tab shows Bob WITHOUT a "(you)" decoration. The
    # easiest check: look for a draw that has Bob's name but NOT "(you)".
    all0 = await _all_drawn_strings(page0)
    bob_lines_on_alice_tab = [s for s in all0 if "Bob" in s]
    assert bob_lines_on_alice_tab, f"FAIL: Alice tab never drew Bob's name; got {all0[:20]}"
    bob_with_you_on_alice_tab = [s for s in bob_lines_on_alice_tab if "(you)" in s]
    assert not bob_with_you_on_alice_tab, (
        f"FAIL: on Alice's tab, Bob's plaque was rendered with '(you)' decoration: "
        f"{bob_with_you_on_alice_tab}. The viewer-scope fix has regressed."
    )
    print(f"  PASS: Alice's tab shows {len(bob_lines_on_alice_tab)} Bob-draws, none with '(you)'")

    for ctx in contexts:
        await ctx.close()
    await browser.close()


async def main():
    from playwright.async_api import async_playwright
    async with async_playwright() as p:
        await test_you_label_only_on_viewer_seat(p)
        print("\nALL '(you)' LABEL TESTS PASSED")


if __name__ == "__main__":
    with server_running():
        try:
            asyncio.run(main())
        except Exception as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            import traceback; traceback.print_exc()
            sys.exit(1)
