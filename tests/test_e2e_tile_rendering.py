"""End-to-end tile rendering tests using Playwright.

Validates the new authentic tile artwork from `web/tiles.js`. In particular:

  1. NO RED 5s. The 5-dots, 5-bamboo, and 5-characters tiles must contain
     no pixels whose RGB is dominated by red. This includes the body of the
     tile AND the small top-left index badge (which we changed from red to
     neutral grey for exactly this reason).

  2. 7-bamboo has a red stalk on top + green stalks below (per the
     reference image the user supplied).

  3. 8-bamboo has 8 distinct stalks visible (no longer a flat 4x2 grid).

  4. Other tiles still render: 1-bamboo (sparrow) has red+green pixels,
     wind/dragon tiles show non-trivial drawing.

The harness spins up the real web server and uses a single non-emulated
Chromium page that renders each tile onto an off-screen 64x96 canvas via
the same `TileRenderer.draw(...)` entry point the live game uses.
"""

import json
import os
import subprocess
import sys
import time
import urllib.request
from contextlib import contextmanager

# Make Unicode output safe on default Windows console (the page logs may
# contain CJK characters when we report tile faces).
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

from playwright.sync_api import sync_playwright

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERVER  = os.path.join(ROOT, "build-web", "Release", "mahjong_web_server.exe")
WEB_DIR = os.path.join(ROOT, "web")
PORT    = 18091  # avoid collision with other e2e tests
BASE    = f"http://localhost:{PORT}"


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
        proc.terminate()
        try: proc.wait(timeout=4)
        except subprocess.TimeoutExpired: proc.kill()


# A pixel is "red-dominant" if R is meaningfully larger than both G and B.
# We use a generous gap so the warm cream face color (#f4ecd2 = 244,236,210)
# does NOT register as red even though R is the largest channel (delta to
# G is only 8 there). Authentic red ink in the tiles is #b81d24 (184,29,36)
# where R-G > 150 and R-B > 140.
RED_PIXEL_JS = """
function countRedPixels(ctx, x, y, w, h) {
  const data = ctx.getImageData(x, y, w, h).data;
  let n = 0;
  for (let i = 0; i < data.length; i += 4) {
    const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
    if (A < 200) continue;
    if (R - G > 60 && R - B > 60 && R > 130) n++;
  }
  return n;
}
function countGreenPixels(ctx, x, y, w, h) {
  const data = ctx.getImageData(x, y, w, h).data;
  let n = 0;
  for (let i = 0; i < data.length; i += 4) {
    const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
    if (A < 200) continue;
    if (G - R > 30 && G - B > 20 && G > 80) n++;
  }
  return n;
}
function countDarkPixels(ctx, x, y, w, h) {
  const data = ctx.getImageData(x, y, w, h).data;
  let n = 0;
  for (let i = 0; i < data.length; i += 4) {
    const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
    if (A < 200) continue;
    if (R < 80 && G < 80 && B < 80) n++;
  }
  return n;
}
"""


SETUP_JS = """() => {
  const tileW = 80, tileH = 110;
  const cvs = document.createElement('canvas');
  cvs.id = '__tileTestCanvas';
  cvs.width = tileW;
  cvs.height = tileH;
  document.body.appendChild(cvs);
  window.__tile = { w: tileW, h: tileH, ctx: cvs.getContext('2d') };
}"""


def render_tile(page, tile):
    """Draw a single tile on the test canvas and return pixel counts."""
    js = """((tile) => {
  const ctx = window.__tile.ctx;
  const w = window.__tile.w, h = window.__tile.h;
  ctx.clearRect(0, 0, w, h);
  TileRenderer.draw(ctx, tile, 0, 0, w, h, {});
  function countRedPixels(x, y, ww, hh) {
    const data = ctx.getImageData(x, y, ww, hh).data;
    let n = 0;
    for (let i = 0; i < data.length; i += 4) {
      const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
      if (A < 200) continue;
      if (R - G > 60 && R - B > 60 && R > 130) n++;
    }
    return n;
  }
  function countGreenPixels(x, y, ww, hh) {
    const data = ctx.getImageData(x, y, ww, hh).data;
    let n = 0;
    for (let i = 0; i < data.length; i += 4) {
      const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
      if (A < 200) continue;
      if (G - R > 30 && G - B > 20 && G > 80) n++;
    }
    return n;
  }
  function countDarkPixels(x, y, ww, hh) {
    const data = ctx.getImageData(x, y, ww, hh).data;
    let n = 0;
    for (let i = 0; i < data.length; i += 4) {
      const R = data[i], G = data[i+1], B = data[i+2], A = data[i+3];
      if (A < 200) continue;
      if (R < 80 && G < 80 && B < 80) n++;
    }
    return n;
  }
  return {
    red:        countRedPixels(0, 0, w, h),
    green:      countGreenPixels(0, 0, w, h),
    dark:       countDarkPixels(0, 0, w, h),
    redTopHalf: countRedPixels(0, 0, w, Math.floor(h * 0.4)),
    redBotHalf: countRedPixels(0, Math.floor(h * 0.5), w, Math.floor(h * 0.5)),
  };
})"""
    return page.evaluate(js, tile)


def make_suit(suit, rank):
    return {
        "id": f"{suit}-{rank}-#1",
        "key": f"{suit}-{rank}",
        "category": "suit",
        "suit": suit,
        "rank": rank,
    }


def test_no_red_5s(page):
    """Verify 5-dots and 5-bamboo have NO red pixels anywhere, and the top
    numeral on 5-characters is not red (the bottom 萬 is always red, by
    design -- that's not the "red 5" the user objected to)."""
    print("\n=== test_no_red_5s ===")
    failures = []
    for suit in ["dots", "bamboo"]:
        tile = make_suit(suit, 5)
        info = render_tile(page, tile)
        # Allow a tiny anti-aliasing fringe (~5 pixels) but no real red.
        if info["red"] > 5:
            failures.append(f"  FAIL: 5-{suit} has {info['red']} red pixels (expected ~0)")
        else:
            print(f"  PASS: 5-{suit} has only {info['red']} red pixels (no red 5)")
    # 5-characters: check that the top numeral (top 40%) is not red. The
    # bottom 萬 is intentionally red and isn't part of the "red 5" concern.
    info = render_tile(page, make_suit("characters", 5))
    if info["redTopHalf"] > 5:
        failures.append(
            f"  FAIL: 5-characters top numeral has {info['redTopHalf']} red pixels "
            f"(expected ~0); whole-tile red = {info['red']}"
        )
    else:
        print(f"  PASS: 5-characters numeral has only {info['redTopHalf']} red pixels "
              f"(萬 below is intentionally red: {info['redBotHalf']} px)")
    if failures:
        for f in failures: print(f)
        raise AssertionError("red pixels detected on a 5-tile")


def test_7_bamboo_has_red_top_stalk(page):
    """7-bamboo should have a red stalk on top + 6 green stalks below."""
    print("\n=== test_7_bamboo_has_red_top_stalk ===")
    info = render_tile(page, make_suit("bamboo", 7))
    print(f"  red(top 40% of tile) = {info['redTopHalf']}")
    print(f"  red(bottom 50% of tile) = {info['redBotHalf']}")
    print(f"  green (whole tile) = {info['green']}")
    assert info["redTopHalf"] > 30, (
        f"expected red stalk in top of 7B; got only {info['redTopHalf']} red pixels"
    )
    # Bottom half should be green-only (the 6 stalks). Allow tiny fringe.
    assert info["redBotHalf"] < 10, (
        f"expected NO red in bottom of 7B; got {info['redBotHalf']} red pixels"
    )
    assert info["green"] > 200, (
        f"expected lots of green stalks in 7B; got {info['green']} green pixels"
    )
    print("  PASS: 7B = red top + green bottom")


def test_8_bamboo_has_eight_stalks(page):
    """8-bamboo should render 8 distinct stalks (lots of green pixels, no red).

    We don't try to count exact stalks via image analysis; instead we verify
    the tile is dominantly green with no red stalks (a regression that would
    occur if the new fan-pattern code accidentally inherited a red center).
    """
    print("\n=== test_8_bamboo_has_eight_stalks ===")
    info = render_tile(page, make_suit("bamboo", 8))
    print(f"  green = {info['green']}, red = {info['red']}")
    assert info["green"] > 400, (
        f"8B should be densely green; got only {info['green']} green pixels"
    )
    assert info["red"] < 5, (
        f"8B should have no red stalks; got {info['red']} red pixels"
    )
    print("  PASS: 8B has many green pixels, no red stalks")


def test_5_chars_numeral_not_red(page):
    """The top numeral on 5-characters must be black, not red."""
    print("\n=== test_5_chars_numeral_not_red ===")
    info = render_tile(page, make_suit("characters", 5))
    print(f"  redTopHalf = {info['redTopHalf']} (top numeral region)")
    print(f"  redBotHalf = {info['redBotHalf']} (bottom 萬 region; expected lots of red)")
    assert info["redTopHalf"] < 5, (
        f"expected NO red on top half of 5-char tile; got {info['redTopHalf']} red pixels"
    )
    # The bottom 萬 is still red, sanity check it.
    assert info["redBotHalf"] > 80, (
        f"expected 萬 to render red; got only {info['redBotHalf']} red pixels"
    )
    print("  PASS: 5-char numeral is non-red; 萬 still red")


def test_1_bamboo_is_a_bird(page):
    """1-bamboo should render the sparrow: red+green+some other accents."""
    print("\n=== test_1_bamboo_is_a_bird ===")
    info = render_tile(page, make_suit("bamboo", 1))
    print(f"  red = {info['red']}, green = {info['green']}")
    assert info["red"]   > 60, f"sparrow body should be red; got {info['red']}"
    assert info["green"] > 60, f"sparrow wing/tail should be green; got {info['green']}"
    print("  PASS: 1B sparrow renders red + green")


def test_other_5_rank_tiles_render(page):
    """Quick sanity: 5-dots and 5-bamboo still draw SOMETHING (not blank)."""
    print("\n=== test_other_5_rank_tiles_render ===")
    for suit in ["dots", "bamboo"]:
        info = render_tile(page, make_suit(suit, 5))
        total = info["red"] + info["green"] + info["dark"]
        print(f"  5-{suit}: total non-face pixels = {total}")
        assert total > 80, f"5-{suit} appears blank (only {total} non-face pixels)"


def main():
    with server_running():
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            ctx = browser.new_context()
            page = ctx.new_page()
            page.goto(BASE + "/", wait_until="domcontentloaded")
            page.wait_for_function("typeof TileRenderer !== 'undefined'", timeout=5000)
            page.evaluate(SETUP_JS)

            test_no_red_5s(page)
            test_7_bamboo_has_red_top_stalk(page)
            test_8_bamboo_has_eight_stalks(page)
            test_5_chars_numeral_not_red(page)
            test_1_bamboo_is_a_bird(page)
            test_other_5_rank_tiles_render(page)

            ctx.close()
            browser.close()
    print("\nALL TILE RENDERING TESTS PASSED")


if __name__ == "__main__":
    main()
