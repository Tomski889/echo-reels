"""Prepare both Balatro variants from the player's own Steam copy.

    python tools/prepare_balatro.py [--exe PATH\\Balatro.exe]

Balatro.exe is a fused LOVE executable: the game is a zip appended to it. We
extract it (read-only; the Steam install is never touched) into:

  apps/balatro/steam       original game + Echo Arcade touch bridge
  apps/balatro/portmaster  PortMaster handheld build (ports/balatro patches,
                           ported from its tools/patchscript) + the bridge

Re-run after Steam updates Balatro.
"""
import argparse
import hashlib
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ROOT / 'apps'
BRIDGE = ROOT / 'bridge/arcade_bridge.lua'
PM = APPS / 'downloads/pm/ports/balatro/balatro'
# Same order as the patchscript's require loop (each is inserted right after the anchor).
MODULES = ['perf', 'small_screen', 'controls', 'options', 'particle_cleanup', 'cpu_opt', 'rumble',
           'achievements', 'loc_fix', 'text_cleanup']
BALATRO_APPID = '2379780'


def steam_libraries():
    roots = [Path(r'C:\Program Files (x86)\Steam'), Path(r'C:\Program Files\Steam')]
    libs = []
    for root in roots:
        vdf = root / 'steamapps/libraryfolders.vdf'
        if vdf.exists():
            libs += [Path(p.replace('\\\\', '\\')) for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(errors='ignore'))]
        libs.append(root)
    return list(dict.fromkeys(libs))


def find_exe():
    for lib in steam_libraries():
        exe = lib / 'steamapps/common/Balatro/Balatro.exe'
        if exe.exists():
            return exe
    return None


def extract(exe: Path, dest: Path):
    dest.mkdir(parents=True, exist_ok=True)
    for child in dest.iterdir():  # clear contents (the folder itself may be someone's cwd)
        shutil.rmtree(child) if child.is_dir() else child.unlink()
    with zipfile.ZipFile(exe) as z:  # zip readers find the archive appended to the exe
        z.extractall(dest)
    if not (dest / 'main.lua').exists():
        raise SystemExit(f'{exe} does not contain a LOVE game (no main.lua)')


def add_bridge(dest: Path):
    shutil.copy2(BRIDGE, dest / 'arcade_bridge.lua')
    main = dest / 'main.lua'
    text = main.read_text(encoding='utf-8')
    if 'arcade_bridge' not in text:
        # Must run after main.lua has defined love.update and the Game class.
        main.write_text(text.rstrip('\n') + '\n\n-- Echo Arcade (tablet touch bridge; inert outside Echo VR)\nrequire("arcade_bridge")\n', encoding='utf-8')


def sub_exact(path: Path, old: str, new: str, count: int):
    text = path.read_text(encoding='utf-8')
    found = text.count(old)
    if found != count:
        raise SystemExit(f'PortMaster patch mismatch in {path.name}: expected {count} of {old!r}, found {found}. '
                         'Balatro may have updated; update the PortMaster port.')
    path.write_text(text.replace(old, new), encoding='utf-8')


def portmaster(dest: Path):
    """tools/patchscript with LAYOUT=small PERFORMANCE=on FONT=nunito."""
    if not (PM / 'patches').is_dir():
        raise SystemExit(f'PortMaster port not found at {PM}')
    g = dest / 'globals.lua'
    text = g.read_text(encoding='utf-8')
    text = text.replace('crt = 70,', 'crt = 0,').replace('bloom = 1', 'bloom = 0').replace("shadows = 'On'", "shadows = 'Off'")
    g.write_text(text, encoding='utf-8')

    (dest / 'portmaster').mkdir(exist_ok=True)
    main = dest / 'main.lua'
    lines = main.read_text(encoding='utf-8').split('\n')
    anchor = next((i for i, line in enumerate(lines) if 'require "challenges"' in line), None)
    if anchor is None:
        raise SystemExit('PortMaster patch mismatch: main.lua has no require "challenges"')
    for module in MODULES:
        shutil.copy2(PM / 'patches' / f'{module}.lua', dest / 'portmaster' / f'{module}.lua')
    # sed '/require "challenges"/a require "x"' in loop order puts each new line directly after the anchor.
    for module in MODULES:
        lines.insert(anchor + 1, f'require "portmaster/{module}"')
    main.write_text('\n'.join(lines), encoding='utf-8')

    es = dest / 'localization/es_419.lua'
    if es.exists():
        es.write_text(re.sub(r'(?m)^\s*a_mult=".*$', '            a_mult="+#1# Multi.",', es.read_text(encoding='utf-8')), encoding='utf-8')

    game = dest / 'game.lua'
    sub_exact(game, "love.graphics.setShader( G.SHADERS['CRT'])",
              "if G.SETTINGS.GRAPHICS.crt > 0 then love.graphics.setShader( G.SHADERS['CRT']) else love.graphics.setShader() end", 1)
    card = dest / 'cardarea.lua'
    text = card.read_text(encoding='utf-8')
    if text.count('(G.SETTINGS.reduced_motion and 0 or 1)*') != 11:
        raise SystemExit('PortMaster patch mismatch in cardarea.lua')
    text = re.sub(r'\(G\.SETTINGS\.reduced_motion and 0 or 1\)\*(0\.\d+\*math\.sin\([^)]*\))',
                  r'(G.SETTINGS.reduced_motion and 0 or \1)', text)
    if text.count('(G.SETTINGS.reduced_motion and 0 or 1)*') != 0 or text.count('(G.SETTINGS.reduced_motion and 0 or 0.') != 11:
        raise SystemExit('PortMaster patch mismatch in cardarea.lua (after)')
    card.write_text(text, encoding='utf-8')

    t = dest / 'engine/text.lua'
    text = t.read_text(encoding='utf-8')
    for needle in ('if self.config.quiver then', 'if self.config.rotate then letter.r =',
                   'if self.config.float then letter.offset.y =', 'if self.config.bump then letter.offset.y ='):
        if text.count(needle) != 1:
            raise SystemExit(f'PortMaster patch mismatch in engine/text.lua: {needle}')
    text = text.replace('if self.config.quiver then', 'if self.config.quiver and not G.SETTINGS.reduced_motion then')
    text = text.replace('(G.SETTINGS.reduced_motion and 0 or 1)*0.02*math.sin(2*G.TIMERS.REAL+k)',
                        '(G.SETTINGS.reduced_motion and 0 or 0.02*math.sin(2*G.TIMERS.REAL+k))')
    text = re.sub(r'if self\.config\.float then letter\.offset\.y = .*',
                  'if self.config.float then letter.offset.y = (G.SETTINGS.reduced_motion and 0 or math.sqrt(self.scale)*(2+(self.font.FONTSCALE/G.TILESIZE)*2000*math.sin(2.666*G.TIMERS.REAL+200*k))) + 60*(letter.scale-1) end', text)
    text = re.sub(r'if self\.config\.bump then letter\.offset\.y = .*',
                  'if self.config.bump then letter.offset.y = (G.SETTINGS.reduced_motion and 0 or self.bump_amount*math.sqrt(self.scale)*7*math.max(0, (5+self.bump_rate)*math.sin(self.bump_rate*G.TIMERS.REAL+200*k) - 3 - self.bump_rate)) end', text)
    t.write_text(text, encoding='utf-8')

    crt = dest / 'resources/shaders/CRT.fs'
    text = crt.read_text(encoding='utf-8')
    fast = ('{ if (crt_intensity <= 0.000001 && noise_fac <= 0.000001 && glitch_intensity <= 0.000001) { MY_HIGHP_OR_MEDIUMP vec2 ftc = (tc*2.0 - vec2(1.0))*scale_fac; '
            'ftc += (ftc.yx*ftc.yx)*ftc*(distortion_fac - 1.0); MY_HIGHP_OR_MEDIUMP number fmask = (1.0 - smoothstep(1.0-feather_fac,1.0,abs(ftc.x) - BUFF))*(1.0 - smoothstep(1.0-feather_fac,1.0,abs(ftc.y) - BUFF)); '
            'ftc = (ftc + vec2(1.0))/2.0; MY_HIGHP_OR_MEDIUMP vec4 fcol = Texel(tex, ftc); fcol.rgb = (fcol.rgb - vec3(0.55))*1.14 + vec3(0.5); fcol.a = 1.0; return fcol*fmask; }')
    text, n = re.subn(r'(vec4 effect\(vec4 color, Image tex, vec2 tc, vec2 pc\)[^\n]*\n)\{', lambda m: m.group(1) + fast, text, count=1)
    if n != 1:
        raise SystemExit('PortMaster patch mismatch in CRT.fs')
    crt.write_text(text, encoding='utf-8')
    sub_exact(dest / 'resources/shaders/background.fs', 'i < 5; i++', 'i < 2; i++', 1)
    shutil.copy2(PM / 'resources/fonts/Nunito-Black.ttf', dest / 'resources/fonts/m6x11plus.ttf')
    shutil.copy2(PM / 'licenses/LICENSE.Nunito_OFL.txt', dest / 'LICENSE.Nunito_OFL.txt')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', type=Path)
    args = ap.parse_args()
    exe = args.exe or find_exe()
    if not exe or not exe.exists():
        print('Balatro.exe not found in any Steam library. Install Balatro from Steam, or pass --exe.')
        return 1
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    print('Using', exe, digest[:16])
    for name, patch in (('steam', False), ('portmaster', True)):
        dest = APPS / 'balatro' / name
        extract(exe, dest)
        if patch:
            portmaster(dest)
        add_bridge(dest)
        # Native helpers from the player's install: https for both, Steam API for the Steam copy.
        for name in ('https.dll',) + (() if patch else ('luasteam.dll', 'steam_api64.dll', 'steam_appid.txt')):
            if (exe.parent / name).exists():
                shutil.copy2(exe.parent / name, dest / name)
        (dest / 'echo_arcade_source.txt').write_text(f'{exe}\n{digest}\n')
        print('Prepared', dest)
    return 0


if __name__ == '__main__':
    sys.exit(main())
