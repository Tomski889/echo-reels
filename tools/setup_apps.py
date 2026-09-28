"""Download everything Echo Arcade needs that is not in this repository.

    python tools/setup_apps.py

  vendor/Doom-on-EchoVR   tablet research + package tools (pinned commit), also the shareware DOOM1.WAD
  apps/love               LOVE 11.5 win64 (runs Balatro from your own Steam copy)
  apps/retroarch          RetroArch stable (portable) + a starter set of cores
  apps/roms/doom          doom1.wad (shareware) + prboom.wad
  apps/downloads/pm       PortMaster's Balatro port (Lua patches only)

Nothing here downloads Balatro or any commercial ROM: bring your own.
"""
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ROOT / 'apps'
DL = APPS / 'downloads'
DOOM_REPO = 'https://github.com/heisthecat31/Doom-on-EchoVR.git'
DOOM_COMMIT = '8531ed84397c64d10e929f6d3836530bfac161d1'
LOVE = 'https://github.com/love2d/love/releases/download/11.5/love-11.5-win64.zip'
RETROARCH = 'https://buildbot.libretro.com/stable/1.22.2/windows/x86_64/RetroArch.7z'
CORES = ['prboom', 'nestopia', 'snes9x', 'gambatte', 'mgba', 'genesis_plus_gx', 'pcsx_rearmed', 'mupen64plus_next']
CORE_URL = 'https://buildbot.libretro.com/nightly/windows/x86_64/latest/{}_libretro.dll.zip'
PRBOOM_WAD = 'https://github.com/libretro/libretro-prboom/raw/master/prboom.wad'


def fetch(url, dest: Path):
    if dest.exists():
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print('downloading', url)
    with urllib.request.urlopen(url) as r, open(dest.with_suffix(dest.suffix + '.part'), 'wb') as f:
        shutil.copyfileobj(r, f)
    dest.with_suffix(dest.suffix + '.part').rename(dest)
    return dest


def git(*args, cwd=None):
    subprocess.run(['git', *args], cwd=cwd, check=True)


def main():
    vendor = ROOT / 'vendor/Doom-on-EchoVR'
    if not vendor.exists():
        git('clone', DOOM_REPO, str(vendor))
    git('checkout', '--quiet', DOOM_COMMIT, cwd=vendor)

    love_zip = fetch(LOVE, DL / 'love-11.5-win64.zip')
    if not (APPS / 'love/love-11.5-win64/love.exe').exists():
        zipfile.ZipFile(love_zip).extractall(APPS / 'love')

    ra = fetch(RETROARCH, DL / 'RetroArch.7z')
    if not (APPS / 'retroarch/retroarch.exe').exists():
        tmp = APPS / 'retroarch_tmp'
        tmp.mkdir(exist_ok=True)
        # Windows' bsdtar reads 7z (including the BCJ2 filter py7zr lacks).
        subprocess.run([r'C:\Windows\System32\tar.exe', '-xf', str(ra), '-C', str(tmp)], check=True)
        shutil.move(str(tmp / 'RetroArch-Win64'), str(APPS / 'retroarch'))
        shutil.rmtree(tmp, ignore_errors=True)
    for core in CORES:
        z = fetch(CORE_URL.format(core), DL / 'cores' / f'{core}_libretro.dll.zip')
        if not (APPS / 'retroarch/cores' / f'{core}_libretro.dll').exists():
            zipfile.ZipFile(z).extractall(APPS / 'retroarch/cores')

    doom = APPS / 'roms/doom'
    doom.mkdir(parents=True, exist_ok=True)
    shutil.copy2(vendor / 'TabletDoom/data/doom1.wad', doom / 'doom1.wad')
    fetch(PRBOOM_WAD, doom / 'prboom.wad')
    (APPS / 'retroarch/system').mkdir(exist_ok=True)
    shutil.copy2(doom / 'prboom.wad', APPS / 'retroarch/system/prboom.wad')
    for system in ('nes', 'snes', 'gb', 'gba', 'genesis', 'psx', 'n64'):
        (APPS / 'roms' / system).mkdir(exist_ok=True)

    pm = DL / 'pm'
    if not pm.exists():
        git('clone', '--depth', '1', '--filter=blob:none', '--sparse', 'https://github.com/PortsMaster/PortMaster-New.git', str(pm))
        git('sparse-checkout', 'set', 'ports/balatro', cwd=pm)
    print('Done. Next: python tools/prepare_balatro.py, build.cmd, python tools/install.py install')
    return 0


if __name__ == '__main__':
    sys.exit(main())
