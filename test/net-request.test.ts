import { test } from "node:test"
import * as assert from "node:assert"
import * as net from "node:net"
import { isInternalAddress, resolveAddress } from "../src/net/address.js"
import { HttpError, NetError } from "../src/net/errors.js"
import { fetchJson } from "../src/net/fetchJson.js"
import { ConcurrencyLimiter } from "../src/net/limiter.js"
import { request, RequestOptions } from "../src/net/request.js"

const LOCAL: RequestOptions = { allowPlaintext: true, allowInternalAddresses: true };

//要求のヘッダ (と Content-Length 分の本文) を受け取ってから handler を呼ぶ生の TCP サーバ
async function rawServer(handler: (socket: net.Socket, request: string) => void) {
	const requests: string[] = [];
	const sockets = new Set<net.Socket>();
	const server = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		socket.on("error", () => {});
		let buf = Buffer.alloc(0);
		let done = false;
		socket.on("data", (chunk) => {
			if (done) return;
			buf = Buffer.concat([buf, chunk]);
			const end = buf.indexOf("\r\n\r\n");
			if (end < 0) return;
			const length = Number(/content-length: (\d+)/i.exec(buf.toString("latin1", 0, end))?.[1] ?? 0);
			if (buf.length < end + 4 + length) return;
			done = true;
			const text = buf.toString("utf8");
			requests.push(text);
			handler(socket, text);
		});
	});
	await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
	const port = (server.address() as net.AddressInfo).port;
	return {
		url: `http://127.0.0.1:${port}`,
		requests,
		close: () => new Promise<void>((resolve) => {
			for (const s of sockets) s.destroy();
			server.close(() => resolve());
		}),
	};
}

async function rejectsWith(promise: Promise<unknown>, code: string, name?: string) {
	await assert.rejects(promise, (ex: unknown) => {
		assert.ok(ex instanceof NetError, `expected NetError, got ${ex}`);
		assert.strictEqual(ex.code, code);
		if (name) assert.strictEqual(ex.name, name);
		return true;
	});
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

test("内部向けアドレスの判定 (IPv4 射影の IPv6 を含む)", () => {
	for (const ip of ["127.0.0.1", "10.1.2.3", "172.16.0.1", "192.168.1.1", "169.254.169.254", "100.64.0.1", "0.0.0.0", "224.0.0.1", "::1", "::", "fd00::1", "fe80::1", "::ffff:127.0.0.1", "::ffff:a01:203", "not-an-ip"]) {
		assert.strictEqual(isInternalAddress(ip), true, ip);
	}
	for (const ip of ["8.8.8.8", "1.1.1.1", "172.32.0.1", "2606:4700::1111", "::ffff:8.8.8.8"]) {
		assert.strictEqual(isInternalAddress(ip), false, ip);
	}
});

test("名前解決: 内部向けにしか解決しない名前は既定で拒否する", async () => {
	await rejectsWith(resolveAddress("127.0.0.1", false), "ADDRESS");
	await rejectsWith(resolveAddress("localhost", false), "ADDRESS");
	assert.strictEqual((await resolveAddress("127.0.0.1", true)).address, "127.0.0.1");
	await rejectsWith(resolveAddress("no-such-host.invalid", true), "ADDRESS");
});

test("同時実行数の上限: 空きを待ち、到着順に渡す。待ちは中断で抜ける", async () => {
	const limiter = new ConcurrencyLimiter(2);
	const signal = new AbortController().signal;
	const a = await limiter.acquire("h", signal);
	const b = await limiter.acquire("h", signal);
	const order: string[] = [];
	const c = limiter.acquire("h", signal).then((r) => { order.push("c"); return r; });
	const aborter = new AbortController();
	const d = limiter.acquire("h", aborter.signal);
	const e = limiter.acquire("h", signal).then((r) => { order.push("e"); return r; });
	aborter.abort(new Error("cancelled"));
	await assert.rejects(d, /cancelled/);
	assert.strictEqual(limiter.inUse("h"), 2);
	a();
	a();	//2 回目は効かない
	(await c)();
	b();
	(await e)();
	assert.deepStrictEqual(order, ["c", "e"]);
	assert.strictEqual(limiter.inUse("h"), 0);
	assert.strictEqual(limiter.inUse("other"), 0);
});

test("request: 平文と内部向けアドレスは明示しないと拒否する", async () => {
	await rejectsWith(request("http://127.0.0.1:9/"), "PROTOCOL");
	await rejectsWith(request("http://127.0.0.1:9/", { allowPlaintext: true }), "ADDRESS");
	await rejectsWith(request("https://user:pw@example.com/"), "PROTOCOL");
});

test("request: 送る要求の形と、chunked の細切れ応答", async () => {
	const srv = await rawServer(async (socket) => {
		socket.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Id: 7\r\n\r\n");
		for (const part of ["3\r\nab", "c\r\n", "2\r", "\nde\r\n0\r\n", "\r\n"]) {
			await sleep(5);
			socket.write(part);
		}
	});
	try {
		const res = await request(`${srv.url}/v1/x?y=1`, { ...LOCAL, method: "POST", headers: { "X-Key": "k" }, body: "{\"a\":1}" });
		assert.strictEqual(res.status, 200);
		assert.strictEqual(res.headers["x-id"], "7");
		assert.strictEqual(res.remoteAddress, "127.0.0.1");
		assert.strictEqual(res.tlsSessionReused, false);
		assert.strictEqual(await res.text(), "abcde");
		const sent = srv.requests[0]!;
		assert.ok(sent.startsWith("POST /v1/x?y=1 HTTP/1.1\r\nHost: 127.0.0.1:"), sent);
		for (const line of ["X-Key: k", "Content-Length: 7", "Accept-Encoding: identity", "Connection: close"]) assert.ok(sent.includes(`\r\n${line}\r\n`), line);
		assert.ok(sent.endsWith("\r\n\r\n{\"a\":1}"));
	} finally {
		await srv.close();
	}
});

test("request: 圧縮された応答・上限を超える本文は拒否し、接続を閉じる", async () => {
	let closedByClient = false;
	const srv = await rawServer((socket, req) => {
		socket.on("close", () => { closedByClient = true; });
		if (req.includes("/gzip")) socket.end("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3\r\n\r\nxyz");
		else if (req.includes("/big")) socket.end("HTTP/1.1 200 OK\r\nContent-Length: 2000\r\n\r\n");
		else {
			//終わりのない chunked
			socket.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
			const timer = setInterval(() => socket.write("400\r\n" + "x".repeat(1024) + "\r\n"), 1);
			socket.on("close", () => clearInterval(timer));
		}
	});
	try {
		await rejectsWith(request(`${srv.url}/gzip`, LOCAL), "PROTOCOL");
		await rejectsWith(request(`${srv.url}/big`, { ...LOCAL, maxBodyBytes: 1000 }), "LIMIT");
		const res = await request(`${srv.url}/endless`, { ...LOCAL, maxBodyBytes: 64 * 1024 });
		await rejectsWith(res.text(), "LIMIT");
		await sleep(50);
		assert.strictEqual(closedByClient, true);
	} finally {
		await srv.close();
	}
});

test("request: 期限 (無通信・全体) と中断", async () => {
	const srv = await rawServer((socket, req) => {
		if (req.includes("/stall")) socket.write("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
		else if (req.includes("/drip")) {
			socket.write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n");
			const timer = setInterval(() => socket.write("x"), 20);
			socket.on("close", () => clearInterval(timer));
		}
	});
	try {
		const stalled = await request(`${srv.url}/stall`, { ...LOCAL, idleTimeoutMs: 100 });
		await rejectsWith(stalled.text(), "TIMEOUT", "TimeoutError");

		const dripping = await request(`${srv.url}/drip`, { ...LOCAL, timeoutMs: 200 });
		await rejectsWith(dripping.text(), "TIMEOUT", "TimeoutError");

		const controller = new AbortController();
		const aborted = await request(`${srv.url}/drip`, { ...LOCAL, signal: controller.signal });
		setTimeout(() => controller.abort(), 50);
		await rejectsWith(aborted.text(), "ABORTED", "AbortError");

		const pre = new AbortController();
		pre.abort();
		await rejectsWith(request(`${srv.url}/drip`, { ...LOCAL, signal: pre.signal }), "ABORTED");
	} finally {
		await srv.close();
	}
});

test("request: 接続できない・途中で切れる", async () => {
	const probe = net.createServer();
	await new Promise<void>((resolve) => probe.listen(0, "127.0.0.1", resolve));
	const freePort = (probe.address() as net.AddressInfo).port;
	await new Promise((resolve) => probe.close(resolve));
	await rejectsWith(request(`http://127.0.0.1:${freePort}/`, LOCAL), "CONNECT");

	const srv = await rawServer((socket, req) => {
		if (req.includes("/cut")) socket.end("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
		else socket.end();
	});
	try {
		const cut = await request(`${srv.url}/cut`, LOCAL);
		await rejectsWith(cut.text(), "CLOSED");
		await rejectsWith(request(`${srv.url}/silent`, LOCAL), "CLOSED");
	} finally {
		await srv.close();
	}
});

test("request: リダイレクトは辿らない (3xx をそのまま返す)", async () => {
	const srv = await rawServer((socket) => socket.end("HTTP/1.1 302 Found\r\nLocation: http://169.254.169.254/\r\nContent-Length: 0\r\n\r\n"));
	try {
		const res = await request(`${srv.url}/`, LOCAL);
		assert.deepStrictEqual([res.status, res.headers["location"]], [302, "http://169.254.169.254/"]);
		res.close();
		assert.strictEqual(srv.requests.length, 1);
	} finally {
		await srv.close();
	}
});

test("request: 同時実行数の上限に達したら、空くまで待つ", async () => {
	const holds: net.Socket[] = [];
	const srv = await rawServer((socket) => holds.push(socket));
	const limiter = new ConcurrencyLimiter(1);
	try {
		const first = request(`${srv.url}/1`, { ...LOCAL, limiter });
		const second = request(`${srv.url}/2`, { ...LOCAL, limiter });
		await sleep(50);
		assert.strictEqual(srv.requests.length, 1);	//2 本目はまだ接続していない
		holds[0]!.end("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n1");
		assert.strictEqual(await (await first).text(), "1");
		await sleep(50);
		assert.strictEqual(srv.requests.length, 2);
		holds[1]!.end("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n2");
		assert.strictEqual(await (await second).text(), "2");
		assert.strictEqual(limiter.inUse("127.0.0.1:" + new URL(srv.url).port), 0);
	} finally {
		await srv.close();
	}
});

test("fetchJson: JSON を返し、エラー本文は先頭 500 バイトだけ読む", async () => {
	const srv = await rawServer((socket, req) => {
		if (req.includes("/ok")) {
			const body = JSON.stringify({ echo: JSON.parse(req.substring(req.indexOf("\r\n\r\n") + 4)) });
			socket.end(`HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
		} else {
			socket.write(`HTTP/1.1 500 Internal Server Error\r\nContent-Length: 100000\r\n\r\n${"e".repeat(2000)}`);
		}
	});
	const transport = (url: string, options?: RequestOptions) => request(url, { ...options, ...LOCAL });
	try {
		assert.deepStrictEqual(await fetchJson(`${srv.url}/ok`, { body: { q: "あ" }, transport }), { echo: { q: "あ" } });
		await assert.rejects(fetchJson(`${srv.url}/fail`, { transport }), (ex: unknown) => ex instanceof HttpError && ex.status == 500 && ex.body.length == 500);
	} finally {
		await srv.close();
	}
});
