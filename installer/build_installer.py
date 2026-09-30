"""Builds dist/EchoArcadeReelsSetup.exe: the setup app with payload.zip embedded.

payload.zip holds what tools/install.py needs, laid out like the repository:
  tools/*.py, vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing/*.py, native/generated/,
  dist/EchoArcade.dll, dist/EchoArcade/ArcadeHost.exe, loader/dinput8.dll,
  python/  (the official Windows embeddable Python + zstandard, pillow, texture2ddecoder)

Needs: build.cmd done (dist/), dist/loader/dinput8.dll, and the embeddable Python zip
(python-3.12.x-embed-amd64.zip from python.org) in build/.

Usage: .venv\\Scripts\\python installer\\build_installer.py
"""
import glob
import os
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build'
PACKAGES = ('zstandard', 'PIL', 'texture2ddecoder')


def main():
    embed = sorted(glob.glob(str(BUILD / 'python-3.12.*-embed-amd64.zip')))
    if not embed:
        raise SystemExit('Put python-3.12.x-embed-amd64.zip (python.org) in build\\ first.')
    site = ROOT / '.venv/Lib/site-packages'
    payload = BUILD / 'payload.zip'
    with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def add(src, arc):
            z.write(src, arc)
        for f in sorted((ROOT / 'tools').glob('*.py')):
            add(f, f'tools/{f.name}')
        probe = ROOT / 'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing'
        for f in sorted(probe.glob('*.py')):
            add(f, f'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing/{f.name}')
        for f in sorted((ROOT / 'native/generated').glob('*.h')):
            add(f, f'native/generated/{f.name}')
        add(ROOT / 'dist/EchoArcade.dll', 'dist/EchoArcade.dll')
        add(ROOT / 'dist/EchoArcade/ArcadeHost.exe', 'dist/EchoArcade/ArcadeHost.exe')
        add(ROOT / 'dist/loader/dinput8.dll', 'loader/dinput8.dll')
        # Python: the embeddable distribution, with site-packages enabled
        with zipfile.ZipFile(embed[-1]) as e:
            for info in e.infolist():
                data = e.read(info)
                if info.filename.endswith('._pth'):
                    lines = [l for l in data.decode().splitlines() if l.strip() and not l.startswith('#')]
                    data = ('\n'.join(lines + ['Lib\\site-packages', 'import site']) + '\n').encode()
                z.writestr(f'python/{info.filename}', data)
        for pkg in PACKAGES:
            for f in (site / pkg).rglob('*'):
                if f.is_file() and '__pycache__' not in f.parts:
                    add(f, f'python/Lib/site-packages/{f.relative_to(site).as_posix()}')
    print(f'{payload}: {payload.stat().st_size / 1e6:.1f} MB')

    csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    out = ROOT / 'dist/EchoArcadeReelsSetup.exe'
    icon = ROOT / 'installer/icon.ico'
    cmd = [str(csc), '/nologo', '/target:winexe', '/platform:x64', '/optimize', f'/out:{out}',
           f'/resource:{payload},payload.zip', '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.Core.dll',
           '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
           str(ROOT / 'installer/ArcadeReelsSetup.cs'), str(ROOT / 'installer/Ui.cs')]
    if icon.exists():
        cmd.insert(5, f'/win32icon:{icon}')
    result = subprocess.run(cmd, capture_output=True, text=True)
    print(result.stdout, result.stderr)
    if result.returncode:
        raise SystemExit('compile failed')
    print(f'{out}: {out.stat().st_size / 1e6:.1f} MB')


if __name__ == '__main__':
    sys.exit(main())
