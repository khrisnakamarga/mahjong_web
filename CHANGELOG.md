# Changelog

All notable changes to the Hong Kong Mahjong C++ project, from initial creation to today.

The project began as a from-scratch C++20 port of the user's existing TypeScript
Hong Kong Mahjong implementation (`~\code\hongkong_mahjong`). It now ships a
deterministic core engine, an authoritative room server, a Win32 native GUI, a
browser web client, and Azure Container Apps deployment artifacts.

Format roughly follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Iterations are listed newest first.

---

## [Iteration 13] — 2026-05-22 — Tile redesign + clickable seat-grid lobby

### Added
- **Clickable seat grid on the landing page.** Both the Create and Join cards
  now render a 2x2 grid of seat cards (single column on phones). Each card
  shows a live badge (`Open` / `Taken (by <name>)` / `Your seat`) and a
  one-click **"Join as East / South / West / North"** button.
- **Copy-link button per seat on the Create card** so the host can share each
  private claim URL by message.
- **Live occupancy polling**: lobby polls `GET /api/rooms/<code>` every 3s so
  newly-joined players appear without a page refresh.
- **`tests/test_e2e_seat_grid_lobby.py`** — Playwright e2e covering host-creates
  → 4 cards render → click "Join as East" → enters seat 0; stranger sees
  seat 0 Taken (no Join button on other seats); friend with private URL sees
  seat 2 as "Your seat" and successfully joins.
- **`tests/test_e2e_tile_rendering.py`** — pixel-counts an off-screen canvas to
  prove: no red 5s on any tile, 7-bamboo has a red top stalk, 8-bamboo has
  eight green stalks, 5-characters numeral is not red, 1-bamboo renders as a
  bird.

### Changed
- **Tile artwork redesigned to match the user-supplied reference image**:
  - **Dots**: donut style (outer ring + inner cutout + center bead + 4 cardinal
    tick marks). 1-dot is an ornate teal medallion with 8 petals.
  - **Bamboo**: segmented pill-shaped stalks. **1-bamboo** is the classic
    sparrow (red body + green wing + gold beak + eye + legs). **7-bamboo** =
    1 red stalk on top + 6 green below. **8-bamboo** uses an M-over-W fan
    pattern (4 stalks tilted ±22°/±8° on top, mirrored on bottom).
  - **Characters**: numeral always rendered in black; 萬 stays red.
- **No red 5s anywhere** (per user request): 5-dots all black, 5-bamboo all
  green, 5-characters numeral in black. The corner index label on every tile
  is now neutral grey (`#766a4a`) instead of red, so 5-tiles never show a
  red "5" badge.
- The legacy manual seat-index / token entry form is preserved inside a
  collapsible `<details>` advanced section.

### Fixed
- Existing e2e tests (`test_e2e_you_label.py`, `test_e2e_mobile_ui.py`,
  `test_browser_auto_pass.py`) updated to programmatically open the new
  `<details>` collapse before clicking `#joinRoomBtn`.

---

## [Iteration 12] — 2026-05-22 — Claim precedence + Chicken Hand + viewer-only "(you)"

### Added
- **Claim precedence gate**: server now rejects a Chow/Pong if another
  non-passed seat has a higher-priority claim available (Win > Pong/Kong >
  Chow). Self-healing via AI loop retry. Reject reason: `claim_pending`.
- **Chicken Hand room toggle**: new checkbox in the topbar sends
  `{type:'set_min_fan', value: 0 or 3}` over WS. Room-global like autoPass
  and aiDelayMs; defaults OFF (minFan=3, standard HK).
- `mahjong::claimPriority(LegalAction)` promoted to public API.
- `RoomRecord.minFan` + `setMinFan`/`getMinFan` + snapshot serialization.
- `RoomManager::injectRoomForTest()` test-only hook.
- New unit tests: `testClaimPriorityOrdering`, `testClaimPrecedenceGate`,
  `testChickenHandSetting`.
- New e2e tests: `tests/test_e2e_chicken_hand.py`,
  `tests/test_e2e_you_label.py` (Playwright dual-context: Alice + Bob).

### Fixed
- **`(you)` label scoping**: previously every human player saw `(you)` next to
  every other human; now only the viewer's own seat plaque renders `(you)`.

---

## [Iteration 11] — 2026-05-22 — Mobile touch fixes + chow disambiguation + meld clearance

### Added
- `tests/test_chow_label.mjs` (10 subtests) covering the chow label helper.
- `tests/test_e2e_mobile_ui.py` (2 Playwright iPhone-12 scenarios) using real
  `touchscreen.tap()` and `document.elementFromPoint()`.

### Fixed
- **Mobile touch coords**: tapping a tile on a phone now actually selects that
  tile. Added `eventToDesign(clientX, clientY)` in `setupCanvasInput` to
  rescale CSS px → design px (the canvas is clamped to design width 640 but
  CSS-scales down on small viewports).
- **Chow button disambiguation**: when multiple chow options are available,
  buttons now show e.g. `Chow 3·[4]·5B` (with `[…]` around the claimed tile
  and `B/C/D` suit suffix) instead of generic `Chow`.
- **Meld clearance**: bumped `MELD_EDGE_OFFSET` from 115 → 140 design px so
  open melds are no longer occluded by other UI.

---

## [Iteration 10] — 2026-05-22 — Mobile-friendly web UI + room TTL/eviction + `azd-redeploy.ps1` hardening

### Added
- **Room TTL + size-cap eviction** end-to-end:
  - `RoomRecord.lastActivityAt` bumped on every mutation.
  - `RoomManager::evictIdleRooms(now, ttlFinished, ttlActive, maxRooms)`.
  - Cleanup thread in `WebServer` (runs every `MAHJONG_ROOM_CLEANUP_INTERVAL_SEC`).
  - Env vars: `MAHJONG_ROOM_TTL_FINISHED_SEC` (default 1800),
    `MAHJONG_ROOM_TTL_ACTIVE_SEC` (default 21600),
    `MAHJONG_ROOM_CLEANUP_INTERVAL_SEC` (default 60),
    `MAHJONG_ROOM_MAX` (default 5000).
  - Evicted rooms send `{type:"error",code:"room_evicted"}` then close all
    connections.
  - `testRoomEviction` C++ unit test.
- **Mobile UX improvements**:
  - 44 px tap targets everywhere on phones.
  - `touch-action: manipulation` on canvas/buttons.
  - `@media (hover: none)` to kill sticky hover on touchscreens.
  - Safe-area-inset padding for notched devices.
  - 16 px font on lobby inputs to suppress iOS auto-zoom.
  - Rotate-overlay nudge (dismissible, persisted in sessionStorage).
  - Topbar wraps + compact action panel on narrow viewports.
  - History panel collapsible (toggle in topbar; default hidden on phones).

### Fixed
- **`scripts/azd-redeploy.ps1`** — 5 bugs:
  1. Replaced Unicode arrows (mojibake on Windows PowerShell 5.1) with ASCII.
  2. Added `Invoke-Azd` wrapper checking `$LASTEXITCODE` (since
     `$ErrorActionPreference = 'Stop'` does NOT trap native-exe failures).
  3. Wrapped `Set-Location` in `Push-Location`/`Pop-Location` (try/finally) so
     CWD doesn't leak to the caller's session.
  4. Renamed `Require-Tool` → `Assert-Tool` (PS approved verb).
  5. URI regex now tolerates both quoted and unquoted output.

### Changed
- `docs/rooms-and-architecture.md` updated: removed the "no eviction" warning;
  added the TTL configuration table.

---

## [Iteration 9] — 2026-05-22 — Room-scoped autoPass + win history + lifecycle docs

### Added
- **autoPass is now room-scoped + server-authoritative** (was per-client).
  New WS message `{type:'set_auto_pass', value: bool}`. Broadcast to all
  seats so the toggle stays in sync.
- **Win history log**: `RoomRecord.winHistory` appended on every
  `!=Finished → Finished` transition. Surfaced in `RoomSnapshot.winHistory`
  and rendered in a new collapsible `#winHistory` side panel showing
  winner / source / fan / deltas, with `[you]` marking your own wins.
- `setAutoPass` / `getAutoPass` / `getWinHistory` on `RoomManager`.
- `winSourceLabel` and `historyEntryLabel` helpers (with "South (South)"
  deduplication and viewer-aware `[you]` marking).
- `tests/test_win_history_label.mjs` (10 subtests).
- `tests/test_e2e_autopass_and_history.py` — fresh-room defaults, two-WS
  cross-tab persistence, history growth across NextRound transitions.
- **`docs/rooms-and-architecture.md`** — Mermaid diagrams (architecture
  flowchart, room lifecycle state diagram, 4-player sequence diagram,
  per-seat snapshot scoping), capacity math (~3K–8K rooms per 0.5 GiB
  replica), and a quick-reference table.

### Fixed
- For discard wins, `claimDiscard` now sets
  `conclusion.responsibleSeat = state.lastDiscard->bySeat` so the discarder
  survives `finish()` clearing `lastDiscard`.
- `test_e2e_win_conclusion.py` accepts `conclusion.responsibleSeat` OR
  `snapshot.lastDiscard.bySeat`.

---

## [Iteration 8] — 2026-05-22 — Winning tile + source display + Azure parity files

### Added
- **Winning tile + source label** rendered in both the web conclusion banner
  (large 54×72 tile with a gold glow) AND the Win32 GUI conclusion banner
  (56×76 via `drawTileAt`). Labels read e.g. "Won by self-draw",
  "Won on discard from South", "Won by flower", "Won by robbing kong".
- HTML side-panel conclusion now also shows the source line and
  `Winning tile: <name>`.
- `winSourceLabel(s)` helper in `web/app.js` and `conclusionSourceText(...)`
  helper in `src/gui/main.cpp`.
- `tests/test_win_source_label.mjs` (12 subtests).
- **Azure deployment scaffolding** (staged but not deployed, per user
  constraint):
  - `azure.yaml` — AZD service manifest.
  - `infra/main.bicep` — AZD-compliant: `environmentName` param,
    `azd-env-name` tag everywhere, `azd-service-name: 'web'` on the
    ContainerApp, placeholder image, standard outputs.
  - `infra/main.parameters.json`, `infra/main.json` (compiled ARM),
    `infra/README.md`.
  - `scripts/azd-up.ps1` — dry-run by default; requires `-Deploy` to act.
  - `docs/azure-deployment.md` — full walkthrough.
- `MAHJONG_BUILD_TESTS` CMake option (defaults ON; OFF inside Dockerfile).

### Fixed
- `Dockerfile`: removed `COPY tests ./tests` (tests/ is in `.dockerignore`),
  added `-DMAHJONG_BUILD_TESTS=OFF`.

---

## [Iteration 7] — 2026-05-22 — Auto-pass interrupted by call opportunities

### Changed
- **Auto-pass semantics inverted**: auto-pass now ONLY fires on pass-only
  windows. Any Chow / Pong / Kong / Mahjong opportunity interrupts auto-pass
  so the user can decide manually.
  - Replaced `hasWin` with `hasCallOption` covering win/chow/pong/kong.
- Auto-pass tooltip updated to reflect new semantics.

### Tests
- Inverted unit tests for chow+pass / pong+pass / kong+pass scenarios.
- Added pass-only positive test.

---

## [Iteration 6] — 2026-05-22 — Auto-pass bug fix + AI speed slider

### Added
- **AI speed slider** (0 → 5 s per AI turn, room-global):
  - `RoomRecord.aiDelayMs`, `setAiDelayMs`, `tickAi`, `nextAiDueAt`.
  - Background `aiWorker_` thread in `WebServer`.
  - WS message `{type:'set_ai_delay', delayMs: N}`.
  - Slider widget in the topbar.
- **Version-keyed auto-pass debounce** (`state.lastAutoActVersion`) — replaced
  the buggy time-based `pendingAutoPass`. Naturally resets when a fresh
  snapshot arrives.
- New tests: `tests/test_auto_pass.mjs`, `tests/test_e2e_auto_pass.py`,
  `tests/test_browser_auto_pass.py`, `tests/test_e2e_ai_delay.py`.

### Fixed
- Auto-pass toggle now actually advances claim windows (root cause: the prior
  debounce held lock indefinitely after a single auto-pass).

---

## [Iteration 5] — 2026-05-22 — Web UI matching Win32 layout

### Changed
- **Web UI completely re-rendered to match the polished Win32 GUI**:
  - Wood frame + green felt background.
  - 4 dedicated discard wells (one per seat).
  - Central 180×180 gold-bordered plaque showing prevailing wind kanji,
    Wall/Turn/Phase/Dealer/Min-fan.
  - Per-seat plaques (red badge for East, navy for others; gold glow on
    current turn) with name + score.
  - 6-column discard grid rotated per owner.
  - Bottom hand sorted, just-drawn tile separated to the right with 10 px gap
    and gold highlight.
  - Opponent hands as face-down backs rotated per slot.
  - Layout scale factor (`layout.scale = min(1, h/900)`) so geometry shrinks
    proportionally on smaller viewports.
- Right-side action panel (240 px sidebar above 820 px viewport; stacks below
  the board on narrow screens).
- Status bar simplified (phase/turn moved onto the plaque).

### Fixed
- Flowers/seasons no longer render as `?` — added key/name lookup
  (`flower-plum`, `season-spring`, etc.) since the server omits `rank` for
  those categories.

### Added
- `scripts/visual-smoke.py` — Playwright + websockets headless visual smoke
  driving the board to a populated state and screenshotting.

---

## [Iteration 4] — 2026-05-22 — Online play: web server + browser client

### Added
- **Phase A — server transport layer**:
  - `MAHJONG_BUILD_WEB_SERVER` CMake option + FetchContent for nlohmann/json
    v3.11.3, asio asio-1-24-0 (pinned for Crow compat), Crow v1.2.0.
  - `src/server/json_codec.{hpp,cpp}` — JSON encoders for every enum +
    struct, plus `legalActionFromJson` parser.
  - `src/server/web_server.{hpp,cpp}` — Crow HTTP REST + WebSocket server.
    Routes: `/api/health`, `POST /api/rooms`, `GET /api/rooms/:code`,
    `POST /api/rooms/:code/seats/:seat`, `WS /ws`.
  - `src/server/web_main.cpp` — entry point reading `PORT`, `MAHJONG_WEB_DIR`,
    `MAHJONG_PUBLIC_BASE_URL`.
  - `scripts/build-web-msvc.ps1` — CMake build script (separate from the
    raw-cl `build-msvc.ps1` for the GUI).
- **Phase B — web client**:
  - `web/index.html` — lobby (Create / Join with token / Spectate) + table
    view with `<canvas>` + action bar + auto-pass toggle.
  - `web/style.css` — dark green felt + gold theme, mobile-responsive.
  - `web/tiles.js` — Canvas tile renderer ported from Win32 GDI (rings,
    bamboo, numerals, dragons, flowers).
  - `web/app.js` — WS client, snapshot rendering, click-to-discard, auto-draw,
    auto-pass.
  - URL deep-link support: `/?room=XXX&seat=N&token=YYY`.
- `scripts/ws-smoke.py` — end-to-end Python WS test (create room, claim seats,
  drive actions, verify snapshot versioning).
- Per-seat snapshot visibility: `RoomManager::createSnapshot(room, viewerSeat)`
  masks concealed tiles to non-viewers (and reveals everything on
  `Phase::Finished`). Spectator mode supported (viewer=nullopt).

---

## [Iteration 3] — Authentic tile design + Auto AI

### Added
- **Authentic tile renderer** in `src/gui/main.cpp` matching a real plastic
  mahjong photo:
  - `tileColors` palette (red/green/blue), `drawRingLocal`,
    `drawBambooStickLocal`, `drawDotsPatternLocal`, `drawBambooBird`,
    `drawBambooPatternLocal`, `chineseNumeral`, `tileIndexText`.
  - Dots as colored rings (blue/red/green by rank).
  - Bamboo as vertical sticks with red accents; **1-bamboo as a bird**
    (red body + green wing + yellow beak + eye).
  - 萬 (characters) tiles: Chinese numeral on top (5 = 伍 in red, others
    blue) + 萬 in red below.
  - White dragon as a blue rectangle outline (no kanji).
  - Small red top-left index on every tile.
- **Just-drawn tile is kept at the right end of the hand** (un-sorted) with a
  gold highlight (`highlightMode = 2`).
- **"Auto AI" toggle**: `BS_AUTOCHECKBOX` in the toolbar + 1-second `SetTimer`
  that steps the AI when the human has no pending action; also auto-applies
  pass when the only legal action is `Pass`.
- `scripts/capture-gui.ps1` switched to `PrintWindow(hWnd, hdc, PW_RENDERFULLCONTENT)`
  so screenshots work regardless of window z-order.

---

## [Iteration 2] — Pretty Mahjong GUI v1

### Changed
- **GUI rewritten from EDIT-control text view to custom-painted Win32**:
  - Double-buffered GDI with `WM_PAINT` + `WM_ERASEBKGND`.
  - Wood-frame border + green felt + diagonal faint table lines.
  - Tile rendering with rounded-rect + shadow + Chinese character glyphs
    (`Microsoft JhengHei UI` font).
  - **World-transform rotation per seat** (bottom=0, right=−π/2, top=π,
    left=π/2) via `SetWorldTransform` + `SaveDC`/`RestoreDC`.
  - `slotForSeat(seat) = (seat - viewerSeat + 4) % 4`.
  - Center info plaque + per-seat corner plaques + discard zones (6-wide
    grid, last-discard highlighted) + meld zones.
  - Hand: bottom = face-up clickable; others = face-down (or revealed when
    finished).
  - Action panel on the right with dynamically created Win32 buttons.
  - Conclusion banner at top.
  - `--demo` flag for screenshot reproduction.

---

## [Iteration 1] — C++ Mahjong project bootstrap

### Added
- **From-scratch C++20/CMake port** of the TypeScript `~\code\hongkong_mahjong`
  project (excluding Azure pieces).
- Targets: `mahjong_core`, `mahjong_server`, `mahjong_client`, `mahjong_gui`,
  `mahjong_tests`.
- `src/core/mahjong_core.{hpp,cpp}` — engine, rules, scoring, payment, AI,
  legal-action generation, seat/round state machine. Mirrors the
  game-engine package's behavior deterministically.
- `src/server/room_server.{hpp,cpp}` — authoritative room manager: snapshots,
  private tokens, command validation, AI auto-stepping.
- `src/client/main.cpp` — interactive console client (human vs 3 AI, watch
  4 AI, legal-action prompts, reveal/hide, next round).
- `src/gui/main.cpp` — initial Win32 GUI (toolbar buttons: Play
  East/South/West/North, Watch 4 AI, Step, Auto to End, Next Round, Reveal,
  AI difficulty, Min Fan; legal action buttons; table text view).
- `src/server/main.cpp` — room shell smoke binary.
- `tests/test_core.cpp` — custom test harness (no external test framework).
- `scripts/build-msvc.ps1` — raw `cl` build via VS 2022 Enterprise
  `VsDevCmd.bat` (no `cmake` required on PATH).
- Docs: `README.md`, `docs/architecture.md`, `docs/rules.md`, `.gitignore`.

### Game rules implemented
- **Adjustable minimum Fan** (default 3; enforced for both humans and AI by
  pruning Win from legal actions when below threshold).
- **Fan-to-points table**:
  `0=1, 1=2, 2=4, 3=8, 4=16, 5=24, 6=32, 7=48, 8=64, 9=96, 10=128, 11=192, 12=256, 13+=384`.
- **Point transfer rules**:
  - Self-draw: every other player pays Fan-points to winner.
  - Discard win: discarder pays Fan-points to winner.
  - Robbing a Kong: robbed Kong owner pays `fan × 3` to winner.
  - All-called self-draw (winner's melds all called from others): provider
    of the last meld pays `fan × 3` to winner.
- Added `WinSource::RobbingKong`; `RoundConclusion.responsibleSeat` for
  liability tracking.

---

## Conventions

- **No deployment from this repo.** All Azure pieces are staged to
  `\\tsclient\C\Users\khris\workspace\hongkong_mahjong_cpp` (the user deploys
  from a separate Azure account via `scripts/azd-redeploy.ps1 -Deploy`).
- **Localhost testing only.** All e2e tests run against `localhost:18080` (or
  a per-test ephemeral port).
- **No name-based process kills.** Always `Stop-Process -Id <PID>` (the dev
  environment blocks `-Name`/`taskkill /IM`).
- **Web UI changes need no rebuild.** The server reads `MAHJONG_WEB_DIR` at
  request time; just refresh the browser.
- **C++ changes need a rebuild**: `.\scripts\build-web-msvc.ps1` for the web
  server, `.\scripts\build-msvc.ps1 -Target gui` for the Win32 GUI.

[Iteration 13]: #iteration-13--2026-05-22--tile-redesign--clickable-seat-grid-lobby
[Iteration 12]: #iteration-12--2026-05-22--claim-precedence--chicken-hand--viewer-only-you
[Iteration 11]: #iteration-11--2026-05-22--mobile-touch-fixes--chow-disambiguation--meld-clearance
[Iteration 10]: #iteration-10--2026-05-22--mobile-friendly-web-ui--room-ttleviction--azd-redeployps1-hardening
[Iteration 9]: #iteration-9--2026-05-22--room-scoped-autopass--win-history--lifecycle-docs
[Iteration 8]: #iteration-8--2026-05-22--winning-tile--source-display--azure-parity-files
[Iteration 7]: #iteration-7--2026-05-22--auto-pass-interrupted-by-call-opportunities
[Iteration 6]: #iteration-6--2026-05-22--auto-pass-bug-fix--ai-speed-slider
[Iteration 5]: #iteration-5--2026-05-22--web-ui-matching-win32-layout
[Iteration 4]: #iteration-4--2026-05-22--online-play-web-server--browser-client
[Iteration 3]: #iteration-3--authentic-tile-design--auto-ai
[Iteration 2]: #iteration-2--pretty-mahjong-gui-v1
[Iteration 1]: #iteration-1--c-mahjong-project-bootstrap
