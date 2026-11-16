# Netplay relay (Cloudflare Worker)

Two headsets in a room code play over the Internet through this: each opens
one WebSocket to `wss://<worker>/room/<CODE>`, and every message goes to the
other. Free on Cloudflare's Workers plan; nothing is stored.

Deploy (once, with your Cloudflare account):

    cd lindberghrecomp/tools/netplay-relay
    npm install
    npx wrangler login
    npx wrangler deploy

It prints the address (`https://lindbergh-netplay.<you>.workers.dev`). Put it
in the game's `env.txt` as

    LINDBERGH_NETPLAY_RELAY=wss://lindbergh-netplay.<you>.workers.dev

Local test: `npx wrangler dev` (ws://localhost:8787), then `node test-room.mjs`.
