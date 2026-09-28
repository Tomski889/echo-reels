"""Build the ARCADE tablet tab from the installed game's stock resources.

Offline: reads the game's manifest/packages, writes build/tab/{manifests,packages}
plus native/generated/arcade_tab.h. Never modifies the game folder (see install.py).

The page is a background panel, one streamed screen sprite (a BGRA texture the
runtime overwrites every frame on the GPU) and an offline label. Touch input is a
GRID_COLS x GRID_ROWS grid of invisible poke buttons laid over the screen.
"""
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROBE = ROOT / 'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing'
os.environ.setdefault('ECHOVR_GAME', str(ROOT.parent / 'ready-at-dawn-echo-arena'))
sys.path.insert(0, str(PROBE))
sys.path.insert(0, str(HERE))

import echovr_pkg as P  # noqa: E402
from canvas_edit import Canvas, descriptor, u64  # noqa: E402
from echovr_patch import Patcher, ManifestFile, MID, GAME_MANIFEST, GAME_PACKAGES, verify  # noqa: E402
from build_tools_tab import empty_canvas, ROOT_CANVAS, NAV_CANVAS, LEVEL, ACTOR  # noqa: E402
from arcade_layout import (PAGE_W, PAGE_H, PAGE_X, PAGE_Y, SCREEN_RECT, TEX_W, TEX_H,  # noqa: E402
                           GRID_COLS, GRID_ROWS, TAB_RECT, TAB_LABEL, splash_bgra)

OUT = ROOT / 'build/tab'
GENERATED = ROOT / 'native/generated'
TITLE_DONOR = 0xfdea8aeb3a0f4862
NAV_SOURCE_BUTTON = 0x275876572b742791
PAGE = P.sym('echo_arcade_page_v1')
TEXTURE = P.sym('echo_arcade_screen_v1')
TAB_BUTTON = P.sym('echo_arcade_tab_button_v1')
BGRA_TEMPLATE = 0x84b9a06506af66fe  # stock 128x128 B8G8R8A8_UNORM texture
DXGI_B8G8R8A8_UNORM = 87


def cell_symbol(c, r):
    return P.sym(f'echo_arcade_cell_{c}_{r}_v1')


def texture_pair(stock):
    """Clone the stock BGRA texture header pair, resized to TEX_W x TEX_H."""
    tt, tg = P.typesym('CGTextureResource'), P.typesym('CGTextureResource', True)
    cpu = bytearray(stock.get(tt, BGRA_TEMPLATE))
    gpu = stock.get(tg, BGRA_TEMPLATE)
    if len(cpu) != 256 or struct.unpack_from('<I', cpu, 0xd8)[0] != DXGI_B8G8R8A8_UNORM:
        raise ValueError('Unexpected BGRA template texture')
    pitch, size = TEX_W * 4, TEX_W * TEX_H * 4
    dds = bytearray(gpu[:148])
    if dds[:4] != b'DDS ' or dds[84:88] != b'DX10' or struct.unpack_from('<I', dds, 128)[0] != DXGI_B8G8R8A8_UNORM:
        raise ValueError('Unexpected BGRA template DDS header')
    struct.pack_into('<III', dds, 12, TEX_H, TEX_W, pitch)
    struct.pack_into('<II', cpu, 0xc4, TEX_W, TEX_H)
    struct.pack_into('<II', cpu, 0xe8, TEX_W, TEX_H)
    struct.pack_into('<II', cpu, 0xf4, len(dds) + size, pitch)
    pixels = splash_bgra()
    assert len(pixels) == size
    return bytes(cpu), bytes(dds) + pixels


def fix_texture_count(canvas):
    """Header +0x28 = elements that reference a texture (sprite 3, mask 4, color mask 8).

    Canvas load pre-sizes its "waiting for texture" list from this count; a sprite
    on a canvas that says 0 makes the engine push into an empty list and crash
    (echovr.exe+0x719ccb). Every stock canvas (277/277) follows this rule.
    """
    count = sum(struct.unpack_from('<I', e, 8)[0] in (3, 4, 8) for e in canvas.elements)
    struct.pack_into('<I', canvas.header, 0x28, count)
    return count


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    patcher = Patcher()
    stock = patcher.stock
    base_manifest = GAME_MANIFEST.read_bytes()
    before = ManifestFile(base_manifest)
    cv = P.typesym('CUICanvasResource')
    tt = P.typesym('CGTextureResource')
    if stock.get(cv, PAGE) is not None or (tt, TEXTURE) in before.index:
        raise SystemExit('ARCADE is already installed in this manifest; run install.py restore first.')
    for n, _, _ in stock.by_type(tt):
        d = stock.get(tt, n)
        if len(d) >= 0xd0 and struct.unpack_from('<II', d, 0xc4) == (TEX_W, TEX_H):
            raise SystemExit(f'Screen size {TEX_W}x{TEX_H} is not unique ({n:016x}); the D3D12 streamer relies on it')

    root = Canvas(stock.get(cv, ROOT_CANVAS))
    nav = Canvas(stock.get(cv, NAV_CANVAS))
    donor = Canvas(stock.get(cv, TITLE_DONOR))
    if len(root.elements) != 7 or len(nav.elements) != 9:
        raise SystemExit('Unexpected stock tablet layout (another tablet mod installed?). Restore it first.')
    old_root = [bytes(r) for r in root.elements]
    old_nav = [bytes(r) for r in nav.elements]
    sprite_template = nav.elements[0]
    assert struct.unpack_from('<I', sprite_template, 8)[0] == 3

    # Navigation: label in the free left slot.
    nav_label = nav.label(donor.elements[1], 'echo_arcade_tab_label_v1', TAB_LABEL, TAB_RECT, 26)

    # Page canvas.
    page = empty_canvas(stock.get(cv, TITLE_DONOR), PAGE_W, PAGE_H)
    struct.pack_into('<Q', page.header, 0, PAGE)
    bg = page.append(donor.elements[0], 'echo_arcade_background_v1', (0, 0, PAGE_W, PAGE_H))
    struct.pack_into('<4f', page.elements[bg], 0x78, .02, .02, .03, 1)
    screen = page.append(sprite_template, 'echo_arcade_screen_sprite_v1', SCREEN_RECT)
    row = page.elements[screen]
    struct.pack_into('<4f', row, 0x78, 1, 1, 1, 1)
    struct.pack_into('<Q', row, 0x88, TEXTURE)
    struct.pack_into('<4f', row, 0x90, 0, 0, 1, 1)
    status = page.label(donor.elements[1], 'echo_arcade_status_v1', 'STARTING ARCADE...',
                        (0, PAGE_H - 34, PAGE_W, PAGE_H), 18, hidden=True, capacity=96)
    # Local vertex/index budgets for RenderMT (same sizing rule as stock pages).
    struct.pack_into('<2I', page.header, 0x2c, 256, 384)

    # Root: page child + header strip + title, all hidden until ARCADE is selected.
    child = root.append(root.elements[1], 'echo_arcade_page_child_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H), hidden=True)
    struct.pack_into('<Q', root.elements[child], 0x78, PAGE)
    header = root.append(donor.elements[0], 'echo_arcade_header_background_v1', (54, 22, 870, 112), hidden=True)
    struct.pack_into('<4f', root.elements[header], 0x78, .02, .02, .03, 1)
    title = root.label(donor.elements[1], 'echo_arcade_title_v1', 'ARCADE', (130, 22, 822, 112), 40, hidden=True, capacity=64)
    struct.pack_into('<2I', root.header, 0x2c, 264, 396)
    struct.pack_into('<2I', nav.header, 0x2c, 548, 822)
    for c in (root, nav, page):
        fix_texture_count(c)
        c.validate()
    assert struct.unpack_from('<I', page.header, 0x28)[0] == 1 and struct.unpack_from('<I', nav.header, 0x28)[0] == 9
    assert [bytes(r) for r in root.elements[:7]] == old_root
    assert [bytes(r) for r in nav.elements[:9]] == old_nav

    # Buttons: tab + touch grid, cloned from a stock navigation button.
    bt = P.typesym('CR15ButtonInteractCR')
    original = stock.get(bt, LEVEL)
    stride, rows = P.parse_cr(original)
    if stride != 296:
        raise SystemExit('Unexpected button record size')
    source = next(r for r in rows if u64(r, 8) == ACTOR and u64(r, 0) == NAV_SOURCE_BUTTON)
    s = .3 / 1024

    def add_button(name_sym, rect):
        row = bytearray(source)
        struct.pack_into('<Q', row, 0, name_sym)
        x0, y0, x1, y1 = rect
        struct.pack_into('<2f', row, 0x80, (x0 + x1) / 2 * s, -(y0 + y1) / 2 * s)
        struct.pack_into('<2f', row, 0x9c, (x1 - x0) / 2 * s, (y1 - y0) / 2 * s)
        rows.append(bytes(row))

    for r in rows:
        if u64(r, 8) != ACTOR or not 0x275876572b742791 <= u64(r, 0) <= 0x275876572b742794:
            continue
        x, y = struct.unpack_from('<2f', r, 0x80)
        w, h = struct.unpack_from('<2f', r, 0x9c)
        k = 1 / s
        rect = ((x - w) * k, (-y - h) * k, (x + w) * k, (-y + h) * k)
        t = TAB_RECT
        assert t[2] <= rect[0] or t[0] >= rect[2] or t[3] <= rect[1] or t[1] >= rect[3], ('Overlapping stock tab', rect)
    add_button(TAB_BUTTON, TAB_RECT)
    sx0, sy0, sx1, sy1 = SCREEN_RECT
    cw, ch = (sx1 - sx0) / GRID_COLS, (sy1 - sy0) / GRID_ROWS
    for r in range(GRID_ROWS):
        for c in range(GRID_COLS):
            add_button(cell_symbol(c, r), (PAGE_X + sx0 + c * cw, PAGE_Y + sy0 + r * ch,
                                           PAGE_X + sx0 + (c + 1) * cw, PAGE_Y + sy0 + (r + 1) * ch))
    hdr = bytearray(original[:56])
    descriptor(hdr, 0, len(rows), stride)
    buttons = bytes(hdr) + b''.join(rows)
    assert buttons[56:len(original)] == original[56:]

    tex_cpu, tex_gpu = texture_pair(stock)
    patcher.add_item('CUICanvasResource', False, f'0x{ROOT_CANVAS:016x}', root.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{NAV_CANVAS:016x}', nav.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{PAGE:016x}', page.serialize())
    patcher.add_item('CR15ButtonInteractCR', False, f'0x{LEVEL:016x}', buttons)
    patcher.add_item('CGTextureResource', False, f'0x{TEXTURE:016x}', tex_cpu)
    patcher.add_item('CGTextureResource', True, f'0x{TEXTURE:016x}', tex_gpu)
    while (GAME_PACKAGES / f'{MID}_{patcher.mf.npkg}').exists():
        i = patcher.mf.npkg
        patcher.mf.C.append([i, (GAME_PACKAGES / f'{MID}_{i}').stat().st_size, 0, 0])
        patcher.mf.npkg += 1
    package_index = patcher.mf.npkg
    patcher.build(OUT)

    after = ManifestFile((OUT / 'manifests' / MID).read_bytes())
    overrides = {(t, n) for t, n, _ in patcher.items}
    after_rows = {(a[0], a[1]): (a, b) for a, b in zip(after.A, after.B)}
    for a, b in zip(before.A, before.B):
        if (a[0], a[1]) not in overrides:
            assert after_rows[(a[0], a[1])] == (a, b), 'Unrelated resource changed'
    assert after.C[:len(before.C)] == before.C, 'Existing frame references changed'
    if not verify(OUT):
        raise SystemExit('Package verification failed')

    sha = lambda b: hashlib.sha256(b).hexdigest()
    package_name = f'{MID}_{package_index}'
    meta = dict(schema=1, base_manifest_sha256=sha(base_manifest),
                manifest_sha256=sha((OUT / 'manifests' / MID).read_bytes()),
                package=package_name, package_sha256=sha((OUT / 'packages' / package_name).read_bytes()),
                page=f'{PAGE:016x}', texture=f'{TEXTURE:016x}', tab_button=f'{TAB_BUTTON:016x}',
                grid=[GRID_COLS, GRID_ROWS], texture_size=[TEX_W, TEX_H])
    (OUT / 'arcade_tab.json').write_text(json.dumps(meta, indent=2))

    GENERATED.mkdir(parents=True, exist_ok=True)
    cells = ','.join(f'0x{cell_symbol(c, r):016x}' for r in range(GRID_ROWS) for c in range(GRID_COLS))
    (GENERATED / 'arcade_tab.h').write_text(
        '// Generated by tools/build_arcade_tab.py -- do not edit.\n#pragma once\n#include <cstdint>\n'
        f'constexpr uint64_t ARCADE_TAB=0x{TAB_BUTTON:016x}, ARCADE_PAGE_MARKER=0x{P.sym("echo_arcade_background_v1"):016x};\n'
        f'constexpr uint64_t ARCADE_ROOT_MARKER=0x{P.sym("echo_arcade_page_child_v1"):016x}, ARCADE_NAV_MARKER=0x{P.sym("echo_arcade_tab_label_v1"):016x};\n'
        f'constexpr unsigned ARCADE_PAGE_ELEMENTS={len(page.elements)}, ARCADE_ROOT_ELEMENTS={len(root.elements)}, ARCADE_NAV_ELEMENTS={len(nav.elements)};\n'
        f'constexpr unsigned ARCADE_ROOT_CHILD={child}, ARCADE_ROOT_HEADER={header}, ARCADE_ROOT_TITLE={title}, ARCADE_NAV_LABEL={nav_label};\n'
        f'constexpr unsigned ARCADE_PAGE_SCREEN={screen}, ARCADE_PAGE_STATUS={status};\n'
        f'constexpr unsigned ARCADE_GRID_COLS={GRID_COLS}, ARCADE_GRID_ROWS={GRID_ROWS};\n'
        f'constexpr unsigned ARCADE_TEX_W={TEX_W}, ARCADE_TEX_H={TEX_H};\n'
        f'constexpr uint64_t ARCADE_CELLS[]={{{cells}}};\n')
    print(f'Built ARCADE tab: {len(patcher.items)} resources in {package_name}, '
          f'{GRID_COLS}x{GRID_ROWS} touch grid, {TEX_W}x{TEX_H} BGRA screen.')
    return meta


if __name__ == '__main__':
    build()
