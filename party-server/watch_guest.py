"""Joins a watch party as a guest and prints what the host sends (for testing without a second headset).

    python watch_guest.py <code> [host:port]      (default 127.0.0.1:8787)
"""
import base64
import os
import socket
import struct
import sys

code = sys.argv[1]
address = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1:8787'
host, port = address.split(':')
s = socket.create_connection((host, int(port)))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f'GET /party/{code}?role=guest HTTP/1.1\r\nHost: {address}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
          f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
stream = s.makefile('rb')
status = stream.readline().decode().strip()
print(status, flush=True)
while stream.readline() not in (b'\r\n', b''):
    pass
while True:
    head = stream.read(2)
    if len(head) < 2:
        break
    length = head[1] & 0x7f
    if length == 126:
        length = struct.unpack('>H', stream.read(2))[0]
    elif length == 127:
        length = struct.unpack('>Q', stream.read(8))[0]
    print(stream.read(length).decode('utf-8', 'replace'), flush=True)
