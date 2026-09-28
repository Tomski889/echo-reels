# Echo Arcade

A new **PLAY** tab on Echo VR's hand tablet that runs **Balatro**, **RetroArch** (30+ systems, full menu, plus DOOM), your **movies** and your **Plex** server on the tablet screen, all by touch. It's built for private community servers. Everything runs locally on your PC and nothing is networked: only you see your tablet.

```
┌──────────── Echo VR hand tablet ────────────┐
│  ECHO ARCADE                tap a game      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ BALATRO  │ │ BALATRO  │ │RETROARCH │     │
│  │ Steam    │ │PortMaster│ │ 30+ sys. │     │
│  └──────────┘ └──────────┘ └──────────┘     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ DOOM     │ │ MOVIES   │ │ PLEX     │     │
│  └──────────┘ └──────────┘ └──────────┘     │
│ [PLAY]  (home) (list) (emblem) (friends)    │
└─────────────────────────────────────────────┘
```

| Tile | What it runs | Controls |
| --- | --- | --- |
| Balatro: Steam original | Your own Steam copy, unmodified content, **your Steam saves and achievements** | Tap = click, slide = drag cards |
| Balatro: PortMaster | [PortMaster's handheld build](https://portmaster.games/detail.html?name=balatro) (bigger text, status-bar HUD, effects off), separate saves | Same |
| RetroArch | RetroArch 1.22 + cores for 30+ systems (Atari to PS2/GameCube/Dreamcast, arcade, DOS). Your ROMs appear under **Playlists**, already paired with a core | On-screen RetroPad: D-pad or **analog stick**, ABXY, L/R, L2/R2, Start/Select, MENU |
| DOOM | Shareware DOOM via the prboom core | Same RetroPad |
| Movies | Your video folders (`apps\movies` and Windows *Videos* by default) in mpv | Tap for controls: ±10/30 s, pause, seek bar, volume, subtitles, audio track |
| Plex | Movies and TV from your Plex server, direct play in mpv; resume points sync back to Plex | Link once with a code at plex.tv/link, then browse libraries, *Continue Watching*, shows, seasons and episodes |

Tap **≡** (Balatro) or **HOME** (RetroArch) for Resume / Quit to launcher. See **[docs/ROMS.md](docs/ROMS.md)** for ROM folders and BIOS files.

## How it works

```
echovr.exe
 ├─ EchoLoader (dbgcore.dll) loads plugins\EchoArcade.dll
 │    • hooks the tablet UI: new tab, page switching, touch cells (MinHook)
 │    • D3D12: finds the tab's screen texture when the engine creates it and
 │      copies each new frame into it on the game's own queue
 └─ starts plugins\EchoArcade\ArcadeHost.exe (dies with Echo via a job object)
        • draws the launcher and touch controls
        • captures the game window (Windows.Graphics.Capture) → shared memory
        • touches → Balatro (Lua bridge over UDP 127.0.0.1)
                  → RetroArch (network RetroPad + command port, 127.0.0.1)
                  → mpv (JSON IPC pipe), Plex (plex.tv link + your server)
```

- **Tablet data:** `tools/build_arcade_tab.py` reads your game's own manifest and adds six resources: the root, navigation and page canvases, the button table (the tab plus a 32×18 grid of invisible poke cells), and a 1024×574 BGRA screen texture. Stock resources are verified byte-identical.
- **Balatro:** `tools/prepare_balatro.py` extracts the game from your `Balatro.exe` (a fused LÖVE executable) into `apps/balatro/`. It never writes to the Steam folder. The game then runs on LÖVE 11.5 with a small touch bridge (`bridge/arcade_bridge.lua`) appended to `main.lua`.
- **Supported build:** `echovr.exe` with timestamp `1683152886`. The runtime checks the executable and every hooked function's bytes at startup, and disables itself (logging the reason) on any mismatch.

## Install

Requirements:

- Windows 10/11 x64.
- An Echo VR install with **EchoLoader** (the `dbgcore.dll` + `echoloader.json` setup used by community servers).
- Visual Studio 2022 with *Desktop development with C++*.
- Python 3.10+.
- Balatro on Steam, if you want the Balatro tiles.

```powershell
python -m venv .venv
.venv\Scripts\pip install zstandard==0.25.0
.venv\Scripts\python tools\setup_apps.py        # LÖVE, mpv, RetroArch + 30 cores, DOOM shareware, PortMaster patches
.venv\Scripts\python tools\prepare_balatro.py   # finds Balatro.exe in your Steam libraries
.\build.cmd                                      # EchoArcade.dll, ArcadeHost.exe, runs the IPC test
# close Echo VR, then:
.venv\Scripts\python tools\install.py install --game "C:\path\to\ready-at-dawn-echo-arena"
```

Start Echo VR, open the hand tablet and press **PLAY**, left of the stock tabs. The first open starts the host; after that the launcher appears.

To uninstall, close Echo VR and run `tools\install.py restore`. It puts back the original manifest (hash-checked) and `echoloader.json`, and removes the package and plugin files. Backups are kept in `backups\`.

After changing code, run `build.cmd` and then `tools\install.py update`. That refreshes the plugin, host and config without touching the tablet data. Re-run `prepare_balatro.py` after Steam updates Balatro.

Put your ROMs in `apps\roms\<system>\` (see [docs/ROMS.md](docs/ROMS.md)); the playlists rebuild every time RetroArch opens. Video folders are set by `movies=` in `plugins\EchoArcade\arcade.ini`.

**Adding a ROM set:** `tools\import_roms.py "<set>.zip" <system>` unpacks a set (including zips nested inside it) into `appsoms\<system>\`.

**FOV tweak:** `plugins\EchoArcade\echo_tweaks.ini` sets `[fov] x=` / `y=` multipliers (0.5 to 2.0), the same idea as EchoVR-Haptics' FovMultiplier. The install seeds it from an old `haptics_config.txt` if present and never overwrites your edits.

**Plex privacy:** linking stores a Plex token in `%LOCALAPPDATA%\EchoArcade\plex.json`, never in the repo or the logs. *Unlink Plex* at the bottom of the Plex library list removes it. Streams go straight from your server to this PC.

## Test without a headset

`tools/fake_game.py` stands in for Echo VR: it keeps the heartbeat alive, pokes cells, and saves the tablet frame as a PNG.

```powershell
dist\EchoArcade\ArcadeHost.exe --standalone
.venv\Scripts\python tools\fake_game.py tap 757 423      # DOOM tile
.venv\Scripts\python tools\fake_game.py snap frame.png
```

## Troubleshooting

- The logs are `%LOCALAPPDATA%\EchoArcade\runtime.log` (in-game plugin) and `host.log` (launcher and apps).
- **PLAY tab missing:** check `runtime.log` for `unsupported echovr.exe` or `signature mismatch`. Another tablet mod (e.g. the DOOM/MUSIC tab) uses the same slot; restore that one first.
- **Tab opens but shows the "STARTING…" splash forever:** look for `stream:` lines in `runtime.log`. `screen texture created` and `frames uploaded` should both appear.
- **App windows** sit behind your other windows at the top-left of the desktop. Set `window_mode=offscreen` in `plugins\EchoArcade\arcade.ini` to move them off-screen.

## Status

On the desktop, every tile has been driven end to end through the real host, capture and input paths using `fake_game.py`: launching, touch input, pause menu and clean quit for all four tiles. The in-headset parts (the tab, touch cells and D3D12 texture streaming) still need checking in a lobby. Report `runtime.log` with any issue.

## Credits

- **[heisthecat31/Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR):** the reverse-engineered tablet canvas format, button records, hook addresses and package tools this builds on. `setup_apps.py` fetches it at a pinned commit; it is not redistributed here.
- **[PortMaster](https://github.com/PortsMaster/PortMaster-New):** the Balatro handheld patches; `prepare_balatro.py` ports its patch script.
- **[MinHook](https://github.com/TsudaKageyu/minhook)** (BSD-2), included in `native/vendor/minhook`.
- **[LÖVE](https://love2d.org), [RetroArch/libretro](https://www.libretro.com), [prboom](https://github.com/libretro/libretro-prboom).**
- Balatro © LocalThunk / Playstack. You need your own copy; no game files are included.
