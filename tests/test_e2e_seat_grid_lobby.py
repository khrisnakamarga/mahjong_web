"""End-to-end test for the clickable seat-grid lobby.

Verifies:
  1. After "Create room", four seat cards render with badges showing seat
     status (Open/Your seat).
  2. Each seat card has a "Join as <wind>" button.
  3. Clicking a seat card's join button transitions to the table view.
  4. After one seat is claimed, opening the same room code in another browser
     context shows that seat as "Taken" (with the player's display name) and
     the other seats as "Open" (no token, no join button).
  5. Opening a private seat URL (?room=&seat=&token=) auto-marks that seat as
     "Your seat" with a Join button, while the others render as "Needs invite".

Spawns its own server on port 18093.

Usage:
    python tests\\test_e2e_seat_grid_lobby.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
import urllib.request

from playwright.sync_api import sync_playwright

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_EXE = os.path.join(ROOT, "build-web", "Release", "mahjong_web_server.exe")
WEB_DIR = os.path.join(ROOT, "web")
PORT = 18093
BASE = f"http://localhost:{PORT}"


def _wait_for_server(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"{BASE}/", timeout=1).read()
            return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError(f"server not up on {BASE}")


def main():
    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["MAHJONG_WEB_DIR"] = WEB_DIR
    env["MAHJONG_ROOM_TTL_FINISHED_SEC"] = "86400"
    env["MAHJONG_ROOM_TTL_ACTIVE_SEC"] = "86400"
    proc = subprocess.Popen(
        [SERVER_EXE], cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        _wait_for_server()
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            ctx_host = browser.new_context()
            ctx_friend = browser.new_context()
            ctx_stranger = browser.new_context()

            # ---------------- 1. Host creates a room ----------------
            host = ctx_host.new_page()
            host.goto(f"{BASE}/")
            host.wait_for_selector("#createRoomBtn")
            host.click("#createRoomBtn")
            # createRoomResult un-hides; seat grid populates from claimLinks.
            host.wait_for_selector("#newRoomCode")
            host.wait_for_selector("#createDisplayName", state="visible")
            host.fill("#createDisplayName", "Khris")
            code = host.locator("#newRoomCode").inner_text()
            assert len(code) == 6, f"unexpected room code: {code!r}"
            print(f"  Created room {code}")

            # Wait for all 4 seat cards to render.
            host.wait_for_selector("#seatGrid .seatCard", timeout=5000)
            cards = host.locator("#seatGrid .seatCard")
            assert cards.count() == 4, f"expected 4 seat cards, got {cards.count()}"
            print(f"  4 seat cards rendered for host")

            # Each card should show "Your seat" badge (host owns all 4 tokens).
            # CSS uppercases via text-transform, so compare case-insensitively.
            for i in range(4):
                badge = host.locator(f"#seatGrid .seatCard[data-seat-index='{i}'] .seatBadge")
                txt = badge.inner_text().strip().lower()
                assert txt in ("your seat", "open"), f"seat {i} unexpected badge: {txt!r}"
            print(f"  All seats show host as owner (Your seat) or Open")

            # Each card has a "Join as <wind>" button.
            for i, wind in enumerate(("East", "South", "West", "North")):
                btn = host.locator(f"#seatGrid .seatCard[data-seat-index='{i}'] button.seatJoin")
                assert btn.count() == 1, f"seat {i} missing Join button"
                txt = btn.inner_text()
                assert wind in txt, f"seat {i} button text {txt!r} missing {wind}"
            print(f"  All seats have 'Join as <wind>' buttons")

            # ---------------- 2. Host clicks "Join as East" ----------------
            host.click("#seatGrid .seatCard[data-seat-index='0'] button.seatJoin")
            host.wait_for_selector("#table:not(.hidden)", timeout=5000)
            host.wait_for_function(
                "window.state && window.state.seatIndex === 0",
                timeout=5000,
            )
            print(f"  Host clicked 'Join as East' -> entered table as seat 0")

            # ---------------- 3. Stranger opens the lobby and types the room
            # code. Should see seat 0 as Taken, seats 1-3 as Open with no join
            # button (no token).
            stranger = ctx_stranger.new_page()
            stranger.goto(f"{BASE}/")
            stranger.fill("#joinRoomCode", code)
            # Polling is debounced 400ms. Wait a beat then verify.
            stranger.wait_for_timeout(800)
            stranger.wait_for_selector("#joinSeatGrid:not(.hidden) .seatCard", timeout=5000)
            seat0_badge = stranger.locator(
                "#joinSeatGrid .seatCard[data-seat-index='0'] .seatBadge"
            ).inner_text().strip().lower()
            seat0_status = stranger.locator(
                "#joinSeatGrid .seatCard[data-seat-index='0'] .seatStatus"
            ).inner_text()
            assert seat0_badge == "taken", f"stranger sees seat 0 badge: {seat0_badge!r}"
            assert "Khris" in seat0_status, f"stranger sees seat 0 status: {seat0_status!r}"
            print(f"  Stranger sees seat 0 as Taken by Khris")

            # Seats 1-3: stranger has no token, so they should NOT have a Join
            # button (the card shows a "Needs invite link" hint instead).
            for i in (1, 2, 3):
                badge = stranger.locator(
                    f"#joinSeatGrid .seatCard[data-seat-index='{i}'] .seatBadge"
                ).inner_text().strip().lower()
                assert badge == "open", f"stranger sees seat {i} badge: {badge!r}"
                btn = stranger.locator(
                    f"#joinSeatGrid .seatCard[data-seat-index='{i}'] button.seatJoin"
                )
                assert btn.count() == 0, f"stranger should NOT have Join button on seat {i}"
            print(f"  Stranger sees seats 1-3 as Open with no Join button (needs invite)")

            # ---------------- 4. Friend opens the private seat-2 URL.
            # Should see seat 2 marked "Your seat" with a Join button, plus
            # seat 0 as Taken and seats 1/3 as Open (no token for those).
            # Need to fish seat-2 token out of host's state.
            tokens = host.evaluate("() => window.state.lobbyTokens || {}")
            seat2_token = tokens.get("2")
            assert seat2_token, f"could not retrieve seat-2 token from host state: {tokens}"

            friend = ctx_friend.new_page()
            friend.goto(f"{BASE}/?room={code}&seat=2&token={seat2_token}")
            friend.wait_for_timeout(800)
            friend.wait_for_selector("#joinSeatGrid:not(.hidden) .seatCard", timeout=5000)
            seat2_badge = friend.locator(
                "#joinSeatGrid .seatCard[data-seat-index='2'] .seatBadge"
            ).inner_text().strip().lower()
            assert seat2_badge == "your seat", (
                f"friend with seat-2 token should see 'Your seat'; got {seat2_badge!r}"
            )
            seat2_btn = friend.locator(
                "#joinSeatGrid .seatCard[data-seat-index='2'] button.seatJoin"
            )
            assert seat2_btn.count() == 1, "friend should have Join button on seat 2"
            assert "West" in seat2_btn.inner_text()
            print(f"  Friend with private link sees seat 2 as 'Your seat' with Join button")

            seat0_badge_friend = friend.locator(
                "#joinSeatGrid .seatCard[data-seat-index='0'] .seatBadge"
            ).inner_text().strip().lower()
            assert seat0_badge_friend == "taken", (
                f"friend should also see seat 0 as Taken; got {seat0_badge_friend!r}"
            )

            # ---------------- 5. Friend clicks "Join as West" ----------------
            friend.fill("#joinDisplayName", "Aunt Mei")
            friend.click("#joinSeatGrid .seatCard[data-seat-index='2'] button.seatJoin")
            friend.wait_for_selector("#table:not(.hidden)", timeout=5000)
            friend.wait_for_function(
                "window.state && window.state.seatIndex === 2",
                timeout=5000,
            )
            print(f"  Friend clicked 'Join as West' -> entered table as seat 2")

            browser.close()
        print("\nALL SEAT GRID LOBBY TESTS PASSED")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=3)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
