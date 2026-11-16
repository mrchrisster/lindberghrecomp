// The netplay relay: a Cloudflare Worker with one Durable Object per room.
//
//   wss://<worker>/room/<CODE>   a WebSocket into room CODE (3-8 of A-Z 0-9)
//   https://<worker>/health      "ok"
//
// Every binary message a socket sends goes to the room's other sockets (not
// back to the sender). Nothing is stored, nobody signs in: a room exists while
// someone is in it. At most 4 sockets a room (a fifth is refused), messages of
// at most 2048 bytes, at most 200 messages a second per socket - enough for
// two players' 60 Hz lockstep input, too little to be useful for anything else.
// The game's own protocol (lockstep.c) does the rest: version check,
// settings, inputs with redundancy, state hashes.

const MAX_SOCKETS = 4, MAX_BYTES = 2048, MAX_RATE = 200;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") return new Response("ok\n");
    const m = url.pathname.match(/^\/room\/([A-Za-z0-9]{3,8})$/);
    if (!m) return new Response("not found\n", { status: 404 });
    if (request.headers.get("Upgrade") !== "websocket") return new Response("want a WebSocket\n", { status: 426 });
    const id = env.ROOMS.idFromName(m[1].toUpperCase());
    return env.ROOMS.get(id).fetch(request);
  },
};

export class Room {
  constructor(state) { this.state = state; }

  async fetch(request) {
    if (this.state.getWebSockets().length >= MAX_SOCKETS) return new Response("room full\n", { status: 409 });
    const [client, server] = Object.values(new WebSocketPair());
    this.state.acceptWebSocket(server);
    server.serializeAttachment({ sec: 0, n: 0 });
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, msg) {
    if (typeof msg === "string" || msg.byteLength > MAX_BYTES) return;
    const a = ws.deserializeAttachment() || { sec: 0, n: 0 };
    const sec = Math.floor(Date.now() / 1000);
    if (a.sec !== sec) { a.sec = sec; a.n = 0; }
    if (++a.n > MAX_RATE) { ws.serializeAttachment(a); return; }
    ws.serializeAttachment(a);
    for (const other of this.state.getWebSockets()) {
      if (other !== ws) { try { other.send(msg); } catch (e) { /* gone */ } }
    }
  }

  webSocketClose(ws, code) { try { ws.close(code, "bye"); } catch (e) {} }
  webSocketError(ws) { try { ws.close(1011, "error"); } catch (e) {} }
}
