import { test } from "node:test"
import * as assert from "node:assert"
import * as net from "node:net"
import { AddressInfo } from "node:net"
import { acceptKey, validateUpgrade } from "../src/ws/handshake.js"
import { encodeFrame, FrameParser, OPCODE, ProtocolError } from "../src/ws/frame.js"
import { WebSocketServer, connectWebSocket, WebSocket } from "../src/ws/index.js"

test("acceptKey は RFC 6455 の例と一致する", () => {
	assert.strictEqual(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
});

test("validateUpgrade は不正な要求を拒否する", () => {
	const ok = { upgrade: "websocket", connection: "keep-alive, Upgrade", "sec-websocket-version": "13", "sec-websocket-key": "dGhlIHNhbXBsZSBub25jZQ==" };
	assert.strictEqual(validateUpgrade("GET", ok), null);
	assert.notStrictEqual(validateUpgrade("POST", ok), null);
	assert.notStrictEqual(validateUpgrade("GET", { ...ok, "sec-websocket-version": "8" }), null);
	assert.notStrictEqual(validateUpgrade("GET", { ...ok, "sec-websocket-key": "short" }), null);
});

test("フレームの往復 (7bit / 16bit / 64bit 長、マスク付き)", () => {
	for (const size of [0, 5, 125, 126, 70000]) {
		const payload = Buffer.alloc(size, 0x61);
		const parser = new FrameParser(true, 1024 * 1024);
		const frames = parser.push(encodeFrame(OPCODE.TEXT, payload, true));
		assert.strictEqual(frames.length, 1);
		assert.ok(frames[0]!.payload.equals(payload));
	}
});

test("分割して届いたフレームも組み立てられる", () => {
	const parser = new FrameParser(false, 1024);
	const bytes = encodeFrame(OPCODE.TEXT, Buffer.from("hello"), false);
	assert.strictEqual(parser.push(bytes.subarray(0, 3)).length, 0);
	const frames = parser.push(bytes.subarray(3));
	assert.strictEqual(frames[0]!.payload.toString(), "hello");
});

test("サーバ側はマスク無しフレーム・上限超過・RSV を拒否する", () => {
	assert.throws(() => new FrameParser(true, 1024).push(encodeFrame(OPCODE.TEXT, Buffer.from("x"), false)), ProtocolError);
	assert.throws(() => new FrameParser(true, 10).push(encodeFrame(OPCODE.TEXT, Buffer.alloc(11), true)), (e: any) => e.closeCode == 1009);
	const rsv = encodeFrame(OPCODE.TEXT, Buffer.from("x"), true);
	rsv[0] = rsv[0]! | 0x40;
	assert.throws(() => new FrameParser(true, 1024).push(rsv), ProtocolError);
});

test("64bit 長ヘッダだけで上限超過を検出する(本体を待たない)", () => {
	const header = Buffer.alloc(14);
	header[0] = 0x81;
	header[1] = 0x80 | 127;
	header.writeBigUInt64BE(BigInt(2) ** BigInt(40), 2);
	assert.throws(() => new FrameParser(true, 1024).push(header), (e: any) => e.closeCode == 1009);
});

function startServer(maxPayload?: number): Promise<{ server: WebSocketServer, port: number }> {
	return new Promise((resolve) => {
		const server = new WebSocketServer({ port: 0, host: "127.0.0.1", maxPayload });
		server.on("listening", () => resolve({ server, port: ((server as any).server.address() as AddressInfo).port }));
	});
}

test("サーバとクライアントでテキストの echo と ping/pong ができる", async () => {
	const { server, port } = await startServer();
	server.on("connection", (ws: WebSocket) => {
		ws.on("message", (data: Buffer) => ws.send("echo:" + data.toString()));
	});

	const client = connectWebSocket(`ws://127.0.0.1:${port}/`);
	await new Promise((r) => client.on("open", r));

	const reply = new Promise<string>((r) => client.on("message", (d: Buffer) => r(d.toString())));
	client.send("こんにちは");
	assert.strictEqual(await reply, "echo:こんにちは");

	const pong = new Promise((r) => client.on("pong", r));
	client.ping();
	await pong;

	const closed = new Promise<number>((r) => client.on("close", (code: number) => r(code)));
	client.close(1000);
	assert.strictEqual(await closed, 1000);
	assert.strictEqual(client.readyState, WebSocket.CLOSED);
	server.close();
});

test("上限を超えるメッセージは 1009 で切断される", async () => {
	const { server, port } = await startServer(16);
	const serverClosed = new Promise<number>((r) => server.on("connection", (ws: WebSocket) => ws.on("close", (code: number) => r(code))));

	const client = connectWebSocket(`ws://127.0.0.1:${port}/`);
	await new Promise((r) => client.on("open", r));
	client.send("x".repeat(100));
	assert.strictEqual(await serverClosed, 1009);
	server.close();
});

test("WebSocket 以外の要求は 426、壊れたハンドシェイクは 400", async () => {
	const { server, port } = await startServer();
	const res = await fetch(`http://127.0.0.1:${port}/`);
	assert.strictEqual(res.status, 426);

	const raw = await new Promise<string>((resolve) => {
		const sock = net.connect(port, "127.0.0.1", () => {
			sock.write("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n");
		});
		let data = "";
		sock.on("data", (d) => data += d.toString());
		sock.on("close", () => resolve(data));
	});
	assert.ok(raw.startsWith("HTTP/1.1 400"));
	server.close();
});
