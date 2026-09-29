"""Stand-in for Echo VR when testing ArcadeHost on the desktop (no headset).

    python tools/fake_game.py snap out.png          # save the current tablet frame
    python tools/fake_game.py tap X Y               # poke the screen at pixel X,Y (1024x574)
    python tools/fake_game.py hold X Y SECONDS      # press and hold
    python tools/fake_game.py drag X0 Y0 X1 Y1      # slide a finger across cells
    python tools/fake_game.py serve SECONDS         # just keep the heartbeat alive
    python tools/fake_game.py mode 0|1              # show the ARCADE (0) or SETTINGS (1) tab
    python tools/fake_game.py poster X Y            # a fingertip taps the docked lobby poster at pixel X,Y
    python tools/fake_game.py pdrag X0 Y0 X1 Y1     # a fingertip slides across the poster
    python tools/fake_game.py shot X Y              # a bullet hits the docked poster at pixel X,Y

While a command runs, dock requests from the host (DOCK TO POSTER) are answered as if a
lobby poster were nearby, so the dock buttons can be tried on the desktop.

Every command keeps the game heartbeat and pageVisible alive while it runs.
Offsets match native/shared/arcade_ipc.h (checked by tests/ipc_test.cpp).
"""
import mmap, struct, sys, time, zlib, ctypes

W, H, COLS, ROWS = 1024, 574, 32, 18
FRAME = W * H * 4
SIZE = 8192 + 3 * FRAME
kernel = ctypes.windll.kernel32
kernel.GetTickCount64.restype = ctypes.c_uint64

shm = mmap.mmap(-1, SIZE, tagname=r'Local\EchoArcade.Shared.v2')
# Offsets (tests/ipc_test.cpp checks them against the C++ struct).
LATEST, HOST_BEAT, DOCK_WANT, DOCK_SERIAL = 36, 48, 152, 156
GAME_PID, PAGE_VISIBLE, READING, PAGE_MODE, GAME_BEAT = 160, 164, 168, 172, 176
TOUCH_WRITE, DOCK_DONE, DOCK_STATE, SHOTS, DOCK_TEXT, TOUCHES = 184, 188, 192, 196, 200, 296
if struct.unpack_from('<I', shm, 0)[0] != 0x41524344:
    struct.pack_into('<8I', shm, 0, 0, 2, W, H, W * 4, COLS, ROWS, 0)
    struct.pack_into('<i', shm, LATEST, -1); struct.pack_into('<i', shm, READING, -1)
    struct.pack_into('<I', shm, 0, 0x41524344)
struct.pack_into('<2I', shm, 20, COLS, ROWS)


def beat():
    struct.pack_into('<i', shm, GAME_PID, 4242)
    struct.pack_into('<i', shm, PAGE_VISIBLE, 1)
    struct.pack_into('<q', shm, GAME_BEAT, kernel.GetTickCount64())
    serial = struct.unpack_from('<i', shm, DOCK_SERIAL)[0]
    if serial != struct.unpack_from('<i', shm, DOCK_DONE)[0]:  # answer DOCK / UNDOCK like the game would
        want = struct.unpack_from('<i', shm, DOCK_WANT)[0]
        text = (b'Docked on the nearest poster (2.4 m away). (fake_game)' if want else b'Undocked. (fake_game)')
        shm[DOCK_TEXT:DOCK_TEXT + 96] = text.ljust(96, b'\0')[:96]
        struct.pack_into('<i', shm, DOCK_STATE, 1 if want else 0)
        struct.pack_into('<i', shm, DOCK_DONE, serial)


def wait(seconds):
    end = time.time() + seconds
    while time.time() < end:
        beat(); time.sleep(0.05)


def event(cell, kind, x=0, y=0):
    n = struct.unpack_from('<i', shm, TOUCH_WRITE)[0]
    struct.pack_into('<i', shm, TOUCH_WRITE, n + 1)
    off = TOUCHES + (n % 256) * 16
    struct.pack_into('<I', shm, off, 0)
    struct.pack_into('<III', shm, off + 4, cell, kind, (int(x) & 0xffff) | (int(y) << 16))
    struct.pack_into('<I', shm, off, n + 1)


def touch(cell, down):
    event(cell, 1 if down else 0)


POINT_DOWN, POINT_MOVE, POINT_UP, SHOT = 2, 3, 4, 5


def cell_at(x, y):
    return min(ROWS - 1, int(y * ROWS / H)) * COLS + min(COLS - 1, int(x * COLS / W))


def snap(path):
    beat()
    index = struct.unpack_from('<i', shm, LATEST)[0]
    if index < 0:
        sys.exit('no frame published yet')
    data = shm[8192 + index * FRAME: 8192 + (index + 1) * FRAME]
    rows = []
    for y in range(H):
        line = bytearray(data[y * W * 4:(y + 1) * W * 4])
        line[0::4], line[2::4] = line[2::4], line[0::4]  # BGRA -> RGBA
        rows.append(b'\0' + bytes(line))
    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)
    print('saved', path, 'host alive:', kernel.GetTickCount64() - struct.unpack_from('<q', shm, HOST_BEAT)[0] < 3000)


cmd = sys.argv[1] if len(sys.argv) > 1 else 'serve'
a = [float(v) for v in sys.argv[2:]] if cmd != 'snap' else []
if cmd == 'snap':
    snap(sys.argv[2])
elif cmd == 'tap':
    c = cell_at(a[0], a[1]); beat(); touch(c, True); wait(0.12); touch(c, False); wait(0.2)
elif cmd == 'hold':
    c = cell_at(a[0], a[1]); beat(); touch(c, True); wait(a[2]); touch(c, False); wait(0.2)
elif cmd == 'mode':
    struct.pack_into('<i', shm, PAGE_MODE, int(a[0])); wait(0.3)
elif cmd == 'poster':
    beat(); event(0, POINT_DOWN, a[0], a[1]); wait(0.12); event(0, POINT_UP, a[0], a[1]); wait(0.2)
elif cmd == 'pdrag':
    beat(); event(0, POINT_DOWN, a[0], a[1]); wait(0.05)
    for i in range(1, 11):
        event(0, POINT_MOVE, a[0] + (a[2] - a[0]) * i / 10, a[1] + (a[3] - a[1]) * i / 10); wait(0.04)
    event(0, POINT_UP, a[2], a[3]); wait(0.2)
elif cmd == 'shot':
    beat(); event(2, SHOT, a[0], a[1]); wait(0.3)
elif cmd == 'drag':
    steps = 8; prev = None
    for i in range(steps + 1):
        c = cell_at(a[0] + (a[2] - a[0]) * i / steps, a[1] + (a[3] - a[1]) * i / steps)
        if c != prev:
            touch(c, True)
            if prev is not None: wait(0.03); touch(prev, False)
            prev = c
        wait(0.05)
    touch(prev, False); wait(0.2)
else:
    wait(a[0] if a else 3600)
