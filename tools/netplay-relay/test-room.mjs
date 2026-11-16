// node test-room.mjs [ws://localhost:8787]: two sockets in one room - each gets the other's message, not its own
const base = process.argv[2] || "ws://localhost:8787";
const room = "T" + Math.floor(Math.random() * 1e6);
const open = () => new Promise((ok, bad) => { const w = new WebSocket(`${base}/room/${room}`); w.binaryType = "arraybuffer"; w.onopen = () => ok(w); w.onerror = bad; });
const a = await open(), b = await open();
const got = { a: [], b: [] };
a.onmessage = (e) => got.a.push(new Uint8Array(e.data)[0]);
b.onmessage = (e) => got.b.push(new Uint8Array(e.data)[0]);
a.send(new Uint8Array([1, 2, 3])); b.send(new Uint8Array([9]));
await new Promise((r) => setTimeout(r, 500));
const pass = got.a.join() === "9" && got.b.join() === "1";
console.log(pass ? "PASS" : "FAIL", JSON.stringify(got));
a.close(); b.close();
process.exit(pass ? 0 : 1);
