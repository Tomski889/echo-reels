// Echo Arcade watch party relay. One room per 4-digit party code (a Durable Object). The host sends what it is watching
// (a small JSON message: site link, play time, paused); the room passes it to every guest and keeps the newest one for
// guests who join later. Only links and times pass through here, never video. Nothing is stored on disk.
//
//   wss://<worker>/party/<code>?role=host&key=<random>   the host (one per room; the key keeps others from taking over)
//   wss://<worker>/party/<code>?role=guest               a guest (receives only)
const MAX_MESSAGE = 2048;

export class Room {
  constructor(state) {
    this.state = state;
    this.last = null;  // the host's newest message (lost if the room sleeps; the host repeats it every few seconds)
  }

  async fetch(request) {
    const url = new URL(request.url);
    const host = url.searchParams.get('role') === 'host';
    const key = (url.searchParams.get('key') || '').slice(0, 64);
    if (host) {
      if (!/^[0-9a-f]{16,64}$/.test(key)) return new Response('bad key', { status: 400 });
      const taken = this.state.getWebSockets('host').some(ws => !this.state.getTags(ws).includes('key:' + key));
      if (taken) return new Response('party code in use', { status: 409 });
    }
    const pair = new WebSocketPair();
    this.state.acceptWebSocket(pair[1], host ? ['host', 'key:' + key] : ['guest']);
    if (!host && this.last) pair[1].send(this.last);
    this.countGuests();
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  webSocketMessage(ws, message) {
    if (typeof message !== 'string' || message.length > MAX_MESSAGE || !this.state.getTags(ws).includes('host')) return;
    this.last = message;
    for (const guest of this.state.getWebSockets('guest')) {
      try { guest.send(message); } catch (e) {}
    }
  }

  webSocketClose(ws) {
    try { ws.close(); } catch (e) {}
    this.countGuests(ws);
  }

  webSocketError(ws) {
    this.webSocketClose(ws);
  }

  // Tells the host how many guests are watching
  countGuests(leaving) {
    const guests = this.state.getWebSockets('guest').filter(ws => ws !== leaving).length;
    for (const host of this.state.getWebSockets('host')) {
      try { host.send(JSON.stringify({ type: 'count', guests })); } catch (e) {}
    }
  }
}

export default {
  async fetch(request, env) {
    const match = new URL(request.url).pathname.match(/^\/party\/(\d{4})$/);
    if (!match) return new Response('Echo Arcade watch party relay\n');
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket only', { status: 426 });
    return env.ROOMS.get(env.ROOMS.idFromName(match[1])).fetch(request);
  },
};
