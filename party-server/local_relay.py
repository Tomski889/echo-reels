"""Local stand-in for worker.js (Python standard library only), for testing the watch party on one network.

    python local_relay.py [port]      (default 8787; arcade.ini party_server=http://127.0.0.1:8787)

Same rules as the Cloudflare relay: /party/<4 digits>?role=host&key=<hex> or ?role=guest; the host's messages go to
every guest, and the newest one to guests who join later; the host is told how many guests are watching.
"""
import asyncio
import base64
import hashlib
import json
import re
import struct
import sys
from urllib.parse import parse_qs, urlparse

GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
rooms = {}  # code -> {'host': writer, 'key': str, 'guests': set, 'last': str}


async def read_frame(reader):
    """One text message (fragments joined); None when the connection closes."""
    message = b''
    while True:
        head = await reader.readexactly(2)
        opcode, length = head[0] & 0x0f, head[1] & 0x7f
        if length == 126:
            length = struct.unpack('>H', await reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack('>Q', await reader.readexactly(8))[0]
        mask = await reader.readexactly(4) if head[1] & 0x80 else b'\0\0\0\0'
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(await reader.readexactly(length)))
        if opcode == 8:
            return None
        if opcode in (0, 1, 2):
            message += data
            if head[0] & 0x80:
                return message.decode('utf-8', 'replace')


def frame(text):
    data = text.encode()
    if len(data) < 126:
        return bytes([0x81, len(data)]) + data
    if len(data) < 65536:
        return bytes([0x81, 126]) + struct.pack('>H', len(data)) + data
    return bytes([0x81, 127]) + struct.pack('>Q', len(data)) + data


def send(writer, text):
    try:
        writer.write(frame(text))
    except Exception:
        pass


def count(room):
    if room.get('host'):
        send(room['host'], json.dumps({'type': 'count', 'guests': len(room['guests'])}))


async def handle(reader, writer):
    request = (await reader.readuntil(b'\r\n\r\n')).decode('latin-1')
    lines = request.split('\r\n')
    path = lines[0].split(' ')[1]
    headers = {k.strip().lower(): v.strip() for k, v in (l.split(':', 1) for l in lines[1:] if ':' in l)}
    url = urlparse(path)
    match = re.fullmatch(r'/party/(\d{4})', url.path)
    query = parse_qs(url.query)
    host = query.get('role', [''])[0] == 'host'
    key = query.get('key', [''])[0]

    def refuse(status, text):
        writer.write(f'HTTP/1.1 {status}\r\nContent-Length: {len(text)}\r\nConnection: close\r\n\r\n{text}'.encode())

    if not match or 'sec-websocket-key' not in headers:
        refuse('200 OK' if not match else '426 Upgrade Required', 'Echo Arcade watch party relay (local)\n')
        return writer.close()
    room = rooms.setdefault(match[1], {'host': None, 'key': '', 'guests': set(), 'last': None})
    if host and (not re.fullmatch(r'[0-9a-f]{16,64}', key) or (room['host'] and room['key'] != key)):
        refuse('409 Conflict', 'party code in use')
        return writer.close()
    accept = base64.b64encode(hashlib.sha1((headers['sec-websocket-key'] + GUID).encode()).digest()).decode()
    writer.write(f'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n'.encode())
    print(f'party {match[1]}: {"host" if host else "guest"} joined', flush=True)
    if host:
        room['host'], room['key'] = writer, key
    else:
        room['guests'].add(writer)
        if room['last']:
            send(writer, room['last'])
    count(room)
    try:
        while (message := await read_frame(reader)) is not None:
            if host and len(message) <= 2048:
                room['last'] = message
                print(f'party {match[1]}: {message}', flush=True)
                for guest in list(room['guests']):
                    send(guest, message)
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        if host and room['host'] is writer:
            room['host'] = None
        room['guests'].discard(writer)
        count(room)
        print(f'party {match[1]}: {"host" if host else "guest"} left', flush=True)
        writer.close()


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    server = await asyncio.start_server(handle, '0.0.0.0', port)
    print(f'watch party relay on port {port}', flush=True)
    async with server:
        await server.serve_forever()


asyncio.run(main())
