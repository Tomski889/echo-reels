# Echo Arcade: ideas backlog

These are suggestions only; none of them is built yet. Anything that runs as a Windows window can be captured and driven by touch, so most of these fit the existing host.

## Top three to do next

1. **On-screen keyboard:** makes search possible in Plex, YouTube and a browser, plus renaming saves.
2. **YouTube / Twitch through mpv:** mpv plays these through its built-in YouTube support (yt-dlp), so little new code is needed.
3. **ScummVM:** already a RetroArch core, and point-and-click suits tapping.

## Games

- **Other LÖVE games you own:** *Mari0*, *Move or Die* and similar reuse the Balatro Lua touch bridge almost unchanged.
- **Mouse-only card and puzzle games:** *Slay the Spire*, *Inscryption*, *Luck be a Landlord*, *Dicey Dungeons*, *Mini Metro*, *Baba Is You*. These need a generic touch-to-mouse bridge for regular Windows games.
- **ScummVM:** point-and-click adventures (Monkey Island, Day of the Tentacle, …) from your own copies.
- **Chess / Go vs the computer:** a board drawn by the host, using the Stockfish engine. Good between rounds in a lobby.
- **Emulator extras:** PICO-8 (owned) and the free TIC-80; Cave Story via the NXEngine core (the freeware original); Quake and Wolfenstein 3D engine ports with your own game files.

## Media

- **Jellyfin / Emby:** the same browse-and-play as Plex, for self-hosted servers.
- **YouTube:** browse and play through mpv + yt-dlp; needs the on-screen keyboard for search.
- **Twitch:** watch streams on the wrist in the lobby, through mpv.
- **Music player / Spotify remote:** Windows media controls. The DOOM mod author's MUSIC tab proves the approach.
- **Photo slideshow:** a folder viewer with next/previous.

## Utilities

- **Desktop mirror:** put any window or monitor on the tablet (Discord, browser, guides). Same capture code, with touch mapped to clicks.
- **Web browser:** an embedded WebView2 browser for guides and wikis. Needs the keyboard.
- **Echo stats page:** your match stats from the community server's API. Read-only.
- **Timer / scoreboard:** for private matches and scrims, drawn by the host with no capture needed.

## Tablet upgrades

- **On-screen keyboard** (see top three).
- **Box art in lists:** RetroArch thumbnails and Plex posters instead of text-only rows.
- **Favourites / recently played** row on the launcher.
- **Save states from the pause menu:** quick save/load through RetroArch's command port.
- **Controller passthrough:** Touch controller thumbstick and buttons as the gamepad while the tablet is open. Harder, because it hooks Echo's input.
- **Big screen mode:** detach the view onto a large floating lobby screen. Much harder, because it needs Echo's world rendering, not the tablet UI.
