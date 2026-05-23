"""End-to-end test for the in-room Invite modal and copy-link URL correctness.

Reproduces the deployed-environment bug where the Copy Link button returned a
"local://mahjong/claim?..." URL because MAHJONG_PUBLIC_BASE_URL was unset on
the server. The fix is to always construct URLs client-side from
window.location.origin.

Spawns its own server on port 18094 WITHOUT setting MAHJONG_PUBLIC_BASE_URL so
the bug would reproduce if the client still trusted claimLink.url.

Verifies:
  1. Lobby seat card "Copy link" button copies an http://localhost:18094/?room=...
     URL (NOT local://mahjong/...).
  2. After entering the table, clicking the Invite button opens the modal with
     four per-seat rows + a spectator row.
  3. Each per-seat row shows an http://localhost:18094/?room=...&seat=...&token=...
     URL (NOT local://mahjong/...).
  4. The "Copy all links" button copies a multi-line string containing the
     spectator URL + all 4 seat URLs.
  5. A second browser context that joined via a private seat URL shows only that
     seat's link in the modal (other seats render the "no invite link" stub).

Usage:
    python tests\\test_e2e_invite_links.py
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
PORT = 18094
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
    # Deliberately DO NOT set MAHJONG_PUBLIC_BASE_URL -- this reproduces the
    # deployed config where the server defaults to "local://mahjong" and the
    # client must construct URLs itself.
    env.pop("MAHJONG_PUBLIC_BASE_URL", None)
    proc = subprocess.Popen(
        [SERVER_EXE], cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        _wait_for_server()
        with sync_playwright() as pw:
            browser = pw.chromium.launch()

            # Both contexts get clipboard read+write permission.
            ctx_host = browser.new_context(
                permissions=["clipboard-read", "clipboard-write"],
                base_url=BASE,
            )
            ctx_friend = browser.new_context(
                permissions=["clipboard-read", "clipboard-write"],
                base_url=BASE,
            )

            # ---------------- 1. Host creates a room ----------------
            host = ctx_host.new_page()
            host.goto(f"{BASE}/")
            host.click("#createRoomBtn")
            host.wait_for_selector("#newRoomCode")
            host.fill("#createDisplayName", "Khris")
            code = host.locator("#newRoomCode").inner_text()
            assert len(code) == 6, f"unexpected room code: {code!r}"
            print(f"  Created room {code}")

            # ---------------- 2. Lobby Copy Link should be a real http:// URL.
            # Read the clipboard after clicking the seat 0 Copy link button.
            host.wait_for_selector("#seatGrid .seatCard[data-seat-index='0']")
            # Grab all copy buttons in the create grid. The "Copy link" button
            # is the secondary button inside the card.
            copy_btns = host.locator("#seatGrid .seatCard[data-seat-index='0'] button")
            n = copy_btns.count()
            copy_link_btn = None
            for i in range(n):
                if "copy" in copy_btns.nth(i).inner_text().strip().lower():
                    copy_link_btn = copy_btns.nth(i)
                    break
            assert copy_link_btn is not None, "no Copy link button on seat 0 card"
            copy_link_btn.click()
            host.wait_for_timeout(200)
            clip = host.evaluate("navigator.clipboard.readText()")
            assert clip.startswith(f"{BASE}/?room={code}&seat=0&token="), (
                f"lobby copy link should produce {BASE}/?... but got: {clip!r}"
            )
            assert "local://mahjong" not in clip, (
                f"lobby copy link still contains local://mahjong: {clip!r}"
            )
            print(f"  Lobby Copy link: {clip[:80]}... OK")

            # ---------------- 3. Host joins seat 0 (East) ----------------
            host.click("#seatGrid .seatCard[data-seat-index='0'] button.seatJoin")
            host.wait_for_selector("#table:not(.hidden)", timeout=5000)
            host.wait_for_function(
                "window.state && window.state.seatIndex === 0", timeout=5000
            )
            print(f"  Host entered table as East")

            # ---------------- 4. Invite button + modal open ----------------
            host.wait_for_selector("#inviteBtn:not(.hidden)", timeout=2000)
            host.click("#inviteBtn")
            host.wait_for_selector("#inviteModal:not(.hidden)", timeout=2000)
            modal_room = host.locator("#inviteRoomCode").inner_text().strip()
            assert modal_room == code, f"modal room code: {modal_room!r}"
            print(f"  Invite modal opened for room {code}")

            # Spectator row + 4 seat rows = 5 rows.
            rows = host.locator("#inviteList .inviteRow")
            assert rows.count() == 5, f"expected 5 invite rows, got {rows.count()}"
            print(f"  Modal shows 5 rows (Spectator + 4 seats)")

            # Each row's URL must use http://localhost:18094 (NOT local://...).
            url_inputs = host.locator("#inviteList .inviteRow .inviteUrl")
            seen = []
            for i in range(url_inputs.count()):
                val = url_inputs.nth(i).input_value()
                assert "local://mahjong" not in val, (
                    f"row {i} URL leaked local://: {val!r}"
                )
                assert val.startswith(BASE + "/?room=" + code), (
                    f"row {i} URL not the right host: {val!r}"
                )
                seen.append(val)
            # First row is spectator (no &seat=&token=).
            assert "&seat=" not in seen[0], f"spectator row had seat param: {seen[0]!r}"
            # Subsequent rows have seat 0..3 + token.
            for i in range(4):
                assert f"seat={i}" in seen[i + 1] and "token=" in seen[i + 1], (
                    f"seat row {i} malformed: {seen[i + 1]!r}"
                )
            print(f"  All 5 URLs use {BASE} and have correct shape")

            # ---------------- 5. Copy all links ----------------
            host.click("#inviteCopyAllBtn")
            host.wait_for_timeout(200)
            clip_all = host.evaluate("navigator.clipboard.readText()")
            assert f"Room {code}" in clip_all, f"missing room header: {clip_all!r}"
            assert "Spectator:" in clip_all, f"missing spectator line: {clip_all!r}"
            for wind in ("East", "South", "West", "North"):
                assert wind in clip_all, f"missing {wind}: {clip_all!r}"
            assert clip_all.count(f"{BASE}/?room={code}") == 5, (
                f"expected 5 URL hits (spectator + 4 seats), got: {clip_all!r}"
            )
            assert "local://mahjong" not in clip_all, (
                f"Copy all leaked local://: {clip_all!r}"
            )
            print(f"  Copy all links produces 5 URLs as expected")

            # ---------------- 6. Friend joins via private URL (seat 2). ----
            # Read seat 2's URL from the modal before closing it.
            friend_url = seen[3]  # seen[0] = spectator, seen[1..4] = seats 0..3
            assert "seat=2" in friend_url, friend_url
            host.click("#inviteCloseBtn")
            host.wait_for_selector("#inviteModal", state="hidden", timeout=2000)

            friend = ctx_friend.new_page()
            friend.goto(friend_url)
            friend.wait_for_selector("#joinSeatGrid:not(.hidden) .seatCard", timeout=5000)
            friend.fill("#joinDisplayName", "Family")
            # The friend's seat 2 card should be 'Your seat'. Click its Join btn.
            friend.click("#joinSeatGrid .seatCard[data-seat-index='2'] button.seatJoin")
            friend.wait_for_selector("#table:not(.hidden)", timeout=5000)
            friend.wait_for_function(
                "window.state && window.state.seatIndex === 2", timeout=5000
            )
            print(f"  Friend joined seat 2 via private URL")

            # Open the invite modal; only seat 2 should have a real URL.
            friend.click("#inviteBtn")
            friend.wait_for_selector("#inviteModal:not(.hidden)", timeout=2000)
            f_rows = friend.locator("#inviteList .inviteRow")
            assert f_rows.count() == 5, f"friend: expected 5 rows, got {f_rows.count()}"
            f_inputs = friend.locator("#inviteList .inviteRow .inviteUrl")
            # Row 0 = spectator (always available).
            assert f_inputs.nth(0).input_value().startswith(BASE + "/?room=" + code), (
                "friend spectator row should have URL"
            )
            # Row 1 = seat 0 (no token for friend).
            assert "no invite link" in f_inputs.nth(1).input_value().lower(), (
                f"friend seat 0 should be no-token: {f_inputs.nth(1).input_value()!r}"
            )
            # Row 3 = seat 2 (friend's own).
            f_seat2 = f_inputs.nth(3).input_value()
            assert "seat=2" in f_seat2 and "token=" in f_seat2, (
                f"friend seat 2 URL: {f_seat2!r}"
            )
            assert f_seat2.startswith(BASE + "/"), f_seat2
            # Row 4 = seat 3 (no token).
            assert "no invite link" in f_inputs.nth(4).input_value().lower(), (
                "friend seat 3 should be no-token"
            )
            print(f"  Friend sees only own seat (2) + spectator with URLs")

            browser.close()
        print("PASS: invite links never leak local://mahjong; modal works")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
