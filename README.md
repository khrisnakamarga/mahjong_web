# Hong Kong Mahjong C++

C++20 reimplementation of the non-Azure Hong Kong Mahjong app. The project keeps the same core architecture as the TypeScript version: a deterministic rules engine, authoritative room/server layer, native client entry point, AI policies, and local tests.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If CMake is unavailable but Visual Studio Build Tools are installed:

```powershell
.\scripts\build-msvc.ps1 -Target all
.\mahjong_tests.exe
```

## Binaries

- `mahjong_tests` validates engine, scoring, AI, and room behavior.
- `mahjong_gui` runs the native Windows GUI with buttons, visible table state, legal actions, AI stepping, auto-run, reveal/hide, and next-round controls.
- `mahjong_client` runs the interactive native console client. It supports local human-vs-AI play, four-AI spectator mode, stepping/auto-running AI, next-round flow, and reveal/hide controls.
- `mahjong_server` runs an authoritative local room-manager shell.

## Run the GUI app

```powershell
.\mahjong_gui.exe
```

Controls:

1. Pick **Play East/South/West/North** to play that seat against AI.
2. Pick **Watch 4 AI** to observe an AI-only table.
3. Set **Min Fan** before or during play. Win actions are only legal when the hand meets that minimum; the default is 3 Fan.
4. Use **Step** for one AI tick, **Auto to End** to run until the next human prompt or round end, **Next Round** after a finished round, and **Reveal** to show/hide hidden hands.
5. Human legal actions appear as buttons in the right panel.

For non-interactive GUI smoke validation:

```powershell
.\mahjong_gui.exe --smoke
```

## Run the console app

```powershell
.\mahjong_client.exe
```

Main menu:

1. `Play local game against AI` lets you pick a seat and take legal Mahjong actions when prompted.
2. `Watch four AI players` lets you step or auto-run an AI-only table.
3. `Run one simulation smoke test` runs one complete deterministic AI round and exits back to the menu.

During local play, choose the AI difficulty and minimum Fan first, then choose from the numbered legal actions. Win actions only appear when the hand meets the configured minimum Fan. Use `r` to reveal/hide all hands for inspection, `n` to start the next round after a win/draw, and `q` to return/quit.

For non-interactive validation:

```powershell
.\mahjong_client.exe --simulate
```

Azure deployment artifacts from the original TypeScript project are intentionally not ported.

See `docs\architecture.md` and `docs\rules.md` for design and rules notes.
