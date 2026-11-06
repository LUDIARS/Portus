import * as http from "node:http"
import * as https from "node:https"
import { acceptKey, generateKey } from "./handshake.js"
import { DEFAULT_MAX_PAYLOAD, WebSocket } from "./socket.js"

//ws:// / wss:// へ接続する。戻り値はすぐ返り、接続できたら open を発火する
export function connectWebSocket(url: string, maxPayload: number = DEFAULT_MAX_PAYLOAD): WebSocket {
	const ws = new WebSocket(true, maxPayload);
	const target = new URL(url);
	if (target.protocol != "ws:" && target.protocol != "wss:") {
		throw new Error(`unsupported protocol ${target.protocol}`);
	}

	const key = generateKey();
	const request = (target.protocol == "wss:" ? https : http).request({
		hostname: target.hostname,
		port: target.port || (target.protocol == "wss:" ? 443 : 80),
		path: target.pathname + target.search,
		headers: {
			"Connection": "Upgrade",
			"Upgrade": "websocket",
			"Sec-WebSocket-Version": "13",
			"Sec-WebSocket-Key": key,
		},
	});

	const fail = (err: Error) => {
		ws.readyState = WebSocket.CLOSED;
		if (ws.listenerCount("error") > 0) ws.emit("error", err);
		ws.emit("close", 1006, Buffer.alloc(0));
	};

	request.on("upgrade", (res, socket, head) => {
		if (res.headers["sec-websocket-accept"] != acceptKey(key)) {
			socket.destroy();
			return fail(new Error("invalid Sec-WebSocket-Accept"));
		}
		ws.attach(socket, head);
	});
	request.on("response", (res) => {
		res.resume();
		fail(new Error(`unexpected response ${res.statusCode}`));
	});
	request.on("error", fail);
	request.end();

	return ws;
}
