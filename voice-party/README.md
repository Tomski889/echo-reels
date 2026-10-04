# Experimental Party voice tablet tab

Adds a seventh navigation tab immediately before Settings. It reuses the stock
social icon, labels the selected page PARTY, and streams an isolated Chrome/Edge
browser into the existing tablet screen texture. Party has a separate shared
memory mapping and touch route. Switching to stock or Arcade pages leaves its
browser and voice connections running; Echo shutdown closes the host job.
Party frames are excluded from lobby-poster uploads.

The native renderer, separate host, on-tablet keyboard and browser voice app are
prototype code. Submit this as a **draft PR** until live headset and cloud tests
are complete. No game packages, user credentials, captures or binaries belong
in the source contribution. Build resource changes from each user's own game.

## Build and install

In addition to the main repository requirements, install PyInstaller and Pillow
in `.venv`, and install Node.js 22 or newer. Set `NODE_LICENSE` to the official
license shipped with that Node distribution. The main `build.cmd` invokes this
directory's build script to create `dist/EchoParty` (including a Node runtime for
the local demo). Chrome or Edge is required on the player's PC.

Rebuild the tablet resources before compiling the runtime so generated constants
match the seven-tab layout. Existing six-tab installations need a reinstall,
not only a DLL update; tablet schema is now 3. The installer includes PartyHost
and preserves an existing `party.json` URL during binary updates. Use the normal
repository backup/restore workflow and close Echo before changes. On uninstall,
the host folder may remain; it is inactive without the Arcade plugin.

## Hosting and audio

A blank `plugins/EchoParty/party.json` URL starts a loopback-only demo. Friends on
different PCs must use the same deployed HTTPS service. See `app/README.md` for
the Cloudflare Workers Free deployment and its limits. No public deployment is
included, and this feature does not depend on access to community Echo servers.
Four-person voice uses direct encrypted WebRTC with free STUN and no TURN relay;
restrictive NAT/firewall configurations can fail to connect.

Create a profile, exchange 16-character friend IDs, accept requests, create a
party and invite friends. Microphone capture begins only after Connect microphone.
Mute Echo's ordinary game voice separately if you want party-only chat. Select
the Rift mic in Audio and configure the Rift as Windows' default audio output.
The isolated browser profile and directory are stored in `%LOCALAPPDATA%/EchoParty`.
Deleting browser data loses the signing identity; there is no recovery flow yet.
No voice recording is implemented. Direct peers can learn each other's network
addresses. The default host starts minimized and remains running for voice.

## Validation and limits

The local prototype compiled with clang/MinGW. A staged seven-tab patch passed
canvas capacity, hitbox, untouched-resource and package verification checks.
A simulated game sent touch cells through shared memory to the keyboard,
profile connection, party creation and fake microphone controls; captured PNG
frames were converted and published as BGRA. Packaged PartyHost started and
exited successfully. Backend tests and two-browser fake-audio tests passed.

Those checks do not establish live Rift rendering, real finger alignment,
real microphone/output routing, compatibility with other rendering hooks or
Cloudflare deployment correctness. The integrated upstream MSVC build and
installer also need validation; local tests used a staged standalone build.
Initial service limits: four members, 50 friends, 2,000 profiles and 100 sockets.

The Party web app has its own MIT license. Native modifications remain under
the upstream project's applicable licensing; this directory does not grant
rights to redistribute proprietary game assets.
