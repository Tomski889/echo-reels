"""Install / restore Echo Arcade in an Echo VR folder. Close Echo VR first.

    python tools/install.py status
    python tools/install.py install     # builds the tab from your manifest, backs up, installs
    python tools/install.py update      # after build.cmd: refresh plugin, host and arcade.ini only
    python tools/install.py reinstall   # restore + install: needed when the tablet data changes
    python tools/install.py restore     # puts every original file back
    python tools/install.py configure   # (dev) write dist/EchoArcade/arcade.ini only

What install changes (all backed up under EchoArcade/backups/<time>/):
  _data/.../manifests/48037dc70b0ecab2      patched manifest (tablet resources + the DOCK poster screen)
  _data/.../packages/48037dc70b0ecab2_N     new package with those resources
  bin/win10/echoloader.json                 adds {"file": "EchoArcade.dll"}
  bin/win10/plugins/EchoArcade.dll          runtime plugin
  bin/win10/plugins/EchoArcade/             ArcadeHost.exe + arcade.ini
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME = ROOT.parent / 'ready-at-dawn-echo-arena'
STATE = ROOT / 'install_state.json'
MID = '48037dc70b0ecab2'
EXE_TIMESTAMP, EXE_SIZE = 1683152886, 35852288
TAB_SCHEMA = 2  # bump when build_arcade_tab.py output changes (2: DOCK poster screen)


def sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def paths(game: Path):
    data = game / '_data/5932408047/rad15/win10'
    bin_ = game / 'bin/win10'
    return dict(manifest=data / 'manifests' / MID, packages=data / 'packages', bin=bin_,
                loader=bin_ / 'echoloader.json', plugins=bin_ / 'plugins')


def check_game(game: Path):
    exe = game / 'bin/win10/echovr.exe'
    if not exe.exists():
        raise SystemExit(f'echovr.exe not found under {game}')
    d = exe.read_bytes()[:4096]
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    ts, size = struct.unpack_from('<I', d, pe + 8)[0], struct.unpack_from('<I', d, pe + 24 + 56)[0]
    if (ts, size) != (EXE_TIMESTAMP, EXE_SIZE):
        raise SystemExit(f'Unsupported echovr.exe build (timestamp {ts}); the runtime addresses are for {EXE_TIMESTAMP}.')
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq echovr.exe'], capture_output=True, text=True).stdout
    if 'echovr.exe' in out.lower():
        raise SystemExit('Echo VR is running. Close it first.')


def write_ini(target: Path):
    apps = ROOT / 'apps'
    ini = f"""; Echo Arcade host configuration (written by tools/install.py)
[paths]
love={apps / 'love/love-11.5-win64/love.exe'}
balatro_steam={apps / 'balatro/steam'}
balatro_portmaster={apps / 'balatro/portmaster'}
retroarch={apps / 'retroarch/retroarch.exe'}
doom_core={apps / 'retroarch/cores/prboom_libretro.dll'}
doom_wad={apps / 'roms/doom/doom1.wad'}
roms={apps / 'roms'}
mpv={apps / 'mpv/mpv.exe'}
; video folders for MOVIES, separated by ;  (environment variables allowed)
movies={apps / 'movies'};%USERPROFILE%\\Videos

[host]
; desktop: game windows sit at the top-left of your monitor, behind other windows
; offscreen: moved off the visible desktop (some apps throttle when off screen)
window_mode=desktop
; where the apps' sound goes: output device name fragments separated by |, first match
; wins (default: the Link / Air Link headset). "default" = the Windows default device.
audio_device=Oculus Virtual Audio|Meta Quest|Rift
balatro_port=55410
retroarch_command_port=55355
retroarch_pad_port=55400
"""
    target.mkdir(parents=True, exist_ok=True)
    (target / 'arcade.ini').write_text(ini, encoding='utf-8')


def ensure_loader_entry(plugins):
    # EchoLoader only calls PluginInit for entries that have "args".
    entry = next((e for e in plugins if e.get('file', '').lower() == 'echoarcade.dll'), None)
    if entry is None:
        plugins.append({'file': 'EchoArcade.dll', 'args': {}})
    else:
        entry.setdefault('args', {})


def load_loader(p: Path):
    return json.loads(p.read_text(encoding='utf-8'))


def check_echoloader(p):
    """Other mod tools (e.g. EchoVR-Haptics) replace bin/win10/dbgcore.dll with
    their own proxy; then EchoLoader, and every plugin it loads, silently stops."""
    dll = p['bin'] / 'dbgcore.dll'
    if dll.exists() and b'EchoLoader' in dll.read_bytes():
        return True
    print('WARNING: bin/win10/dbgcore.dll is not EchoLoader (another mod replaced it?).\n'
          '         Echo Arcade will not load until it is restored: download dbgcore.dll from\n'
          '         https://files.echovr.de/updates/ (the Echo VR Installer\'s source) into bin/win10.')
    return False


def install(game: Path):
    check_game(game)
    if STATE.exists():
        raise SystemExit('Already installed (install_state.json exists). Run restore first.')
    p = paths(game)
    for f in (ROOT / 'dist/EchoArcade.dll', ROOT / 'dist/EchoArcade/ArcadeHost.exe'):
        if not f.exists():
            raise SystemExit(f'{f} missing: run build.cmd first')
    sys.path.insert(0, str(ROOT / 'tools'))
    import os
    os.environ['ECHOVR_GAME'] = str(game)
    import build_arcade_tab
    meta = build_arcade_tab.build()
    if meta['base_manifest_sha256'] != sha(p['manifest']):
        raise SystemExit('Manifest changed during build; try again.')
    package = ROOT / 'build/tab/packages' / meta['package']
    if (p['packages'] / meta['package']).exists():
        raise SystemExit(f'{meta["package"]} already exists in the game; refusing to overwrite it.')

    backup = ROOT / 'backups' / time.strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True)
    shutil.copy2(p['manifest'], backup / MID)
    shutil.copy2(p['loader'], backup / 'echoloader.json')

    shutil.copy2(package, p['packages'] / meta['package'])
    shutil.copy2(ROOT / 'build/tab/manifests' / MID, p['manifest'])
    p['plugins'].mkdir(exist_ok=True)
    copy_binaries(p)
    loader = load_loader(p['loader'])
    plugins = loader.setdefault('plugins', [])
    ensure_loader_entry(plugins)
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')

    STATE.write_text(json.dumps(dict(game=str(game), backup=str(backup), package=meta['package'], tab_schema=TAB_SCHEMA,
                                     manifest_sha256=sha(p['manifest']), original_manifest_sha256=meta['base_manifest_sha256']), indent=2))
    check_echoloader(p)
    print(f'Installed. Backups in {backup}')
    print('Start Echo VR normally, open the hand tablet and press the gamepad tab (far left); the gear tab (far right) is SETTINGS.')


def write_tweaks(p):
    """echo_tweaks.ini is yours to edit: created once, never overwritten.

    Seeds the FOV from EchoVR-Haptics' haptics_config.txt if that is present.
    """
    target = p['plugins'] / 'EchoArcade' / 'echo_tweaks.ini'
    if target.exists():
        return
    x = y = 1.0
    old = p['bin'] / 'haptics_config.txt'
    if old.exists():
        for line in old.read_text(errors='ignore').splitlines():
            key, _, value = line.partition('=')
            key = key.strip()
            try:
                if key in ('FovMultiplierX', 'FovMultiplier'):
                    x = float(value)
                if key in ('FovMultiplierY', 'FovMultiplier'):
                    y = float(value)
            except ValueError:
                pass
        # One-axis stretch (e.g. x1.0 y1.5) looks warped in a wide recording window.
        x = y = max(x, y)
    target.write_text(f"""; Echo VR tweaks (read when Echo starts; edit here or on the tablet's SETTINGS tab)
[fov]
; Eye field-of-view multipliers, 0.8 to 2.0 (1.0 = normal). Widens the view the
; game renders: visible in the desktop mirror / recordings.
; Keep x and y equal for natural proportions. Higher values cost GPU (1.4 = ~2x the pixels).
x={x:.2f}
y={y:.2f}

[tablet]
; Tablet size, 0.75 to 2.0 (saved by the SETTINGS tab; applied by a future plugin update)
scale=1.00
""", encoding='utf-8')
    print(f'Created {target.name} (fov x={x:.2f} y={y:.2f})')


def copy_binaries(p):
    shutil.copy2(ROOT / 'dist/EchoArcade.dll', p['plugins'] / 'EchoArcade.dll')
    host_dir = p['plugins'] / 'EchoArcade'
    host_dir.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'dist/EchoArcade/ArcadeHost.exe', host_dir / 'ArcadeHost.exe')
    write_ini(host_dir)
    write_tweaks(p)


def update(game: Path):
    check_game(game)
    if not STATE.exists():
        raise SystemExit('Not installed yet: run install first.')
    state = json.loads(STATE.read_text())
    if state.get('tab_schema', 1) < TAB_SCHEMA:
        raise SystemExit('This version changes the tablet data (DOCK adds a poster screen), so update is not enough.\n'
                         'Close Echo VR and run:  .venv\\Scripts\\python tools\\install.py reinstall')
    p = paths(Path(state['game']))
    copy_binaries(p)
    loader = load_loader(p['loader'])
    ensure_loader_entry(loader.setdefault('plugins', []))
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')
    print('Updated EchoArcade.dll, ArcadeHost.exe and arcade.ini (tablet data unchanged).')
    check_echoloader(p)


def restore(game: Path, force: bool):
    check_game(game)
    if not STATE.exists():
        raise SystemExit('Nothing to restore (no install_state.json).')
    state = json.loads(STATE.read_text())
    p = paths(Path(state['game']))
    backup = Path(state['backup'])
    current = sha(p['manifest'])
    if current != state['manifest_sha256'] and not force:
        raise SystemExit('The manifest was changed after Echo Arcade installed (another mod or a game update?). '
                         'Re-run with --force to restore our backup anyway.')
    shutil.copy2(backup / MID, p['manifest'])
    pkg = p['packages'] / state['package']
    if pkg.exists():
        pkg.unlink()
    loader = load_loader(p['loader'])
    loader['plugins'] = [e for e in loader.get('plugins', []) if e.get('file', '').lower() != 'echoarcade.dll']
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')
    for f in (p['plugins'] / 'EchoArcade.dll',):
        if f.exists():
            f.unlink()
    host_dir = p['plugins'] / 'EchoArcade'
    if host_dir.exists():  # keep the user's echo_tweaks.ini (FOV, tablet size) for a reinstall
        for f in host_dir.iterdir():
            if f.name != 'echo_tweaks.ini':
                shutil.rmtree(f, ignore_errors=True) if f.is_dir() else f.unlink()
        if not any(host_dir.iterdir()):
            host_dir.rmdir()
        else:
            print(f'Kept your settings file {host_dir / "echo_tweaks.ini"} (delete it by hand if you are uninstalling for good).')
    ok = sha(p['manifest']) == state['original_manifest_sha256']
    STATE.unlink()
    print('Restored original manifest' + (' (hash verified).' if ok else ' (WARNING: hash differs from the pre-install manifest).'))


def status(game: Path):
    p = paths(game)
    print('game:', game)
    print('installed:', STATE.exists())
    print('plugin present:', (p['plugins'] / 'EchoArcade.dll').exists())
    print('echoloader dll:', check_echoloader(p))
    print('loader entry:', any(e.get('file', '').lower() == 'echoarcade.dll' for e in load_loader(p['loader']).get('plugins', [])))
    print('manifest sha256:', sha(p['manifest']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['install', 'update', 'reinstall', 'restore', 'status', 'configure'])
    ap.add_argument('--game', type=Path, default=DEFAULT_GAME)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    if a.action == 'install':
        install(a.game)
    elif a.action == 'update':
        update(a.game)
    elif a.action == 'reinstall':
        game = Path(json.loads(STATE.read_text())['game']) if STATE.exists() else a.game
        if STATE.exists():
            restore(game, a.force)
        install(game)
    elif a.action == 'restore':
        restore(a.game, a.force)
    elif a.action == 'status':
        status(a.game)
    else:
        write_ini(ROOT / 'dist/EchoArcade')
        print('wrote', ROOT / 'dist/EchoArcade/arcade.ini')


if __name__ == '__main__':
    main()
