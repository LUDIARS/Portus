import { test } from "node:test"
import * as assert from "node:assert"
import { NetError } from "../src/net/errors.js"
import { ParseEvent, ParserLimits, ResponseParser, serializeRequest } from "../src/net/http1.js"

const LIMITS: ParserLimits = { maxHeaderBytes: 16 * 1024, maxHeaderCount: 100, maxBodyBytes: 1024 * 1024 };

function parseAll(raw: string | Buffer, limits: ParserLimits = LIMITS, splitAt: number[] = []): { status: number, headers: Record<string, string>, body: string, ended: boolean } {
	const bytes = typeof raw == "string" ? Buffer.from(raw, "latin1") : raw;
	const parser = new ResponseParser(limits);
	const events: ParseEvent[] = [];
	let from = 0;
	for (const at of [...splitAt, bytes.length]) {
		events.push(...parser.push(bytes.subarray(from, at)));
		from = at;
	}
	const head = events.find((e) => e.type == "head");
	const body = Buffer.concat(events.flatMap((e) => e.type == "data" ? [e.chunk] : []));
	return {
		status: head && head.type == "head" ? head.head.status : 0,
		headers: head && head.type == "head" ? head.head.headers : {},
		body: body.toString("utf8"),
		ended: events.some((e) => e.type == "end"),
	};
}

function rejects(raw: string, code: string, limits: ParserLimits = LIMITS) {
	assert.throws(() => {
		const parser = new ResponseParser(limits);
		parser.push(Buffer.from(raw, "latin1"));
		parser.finish();
	}, (ex: unknown) => ex instanceof NetError && ex.code == code, raw.substring(0, 60));
}

test("要求の組み立て: Host・Content-Length・圧縮なし・接続を閉じる", () => {
	const get = serializeRequest("GET", new URL("https://api.example.com/v1/models?limit=1"), { "Accept": "application/json" }, null).toString("latin1");
	assert.strictEqual(get, "GET /v1/models?limit=1 HTTP/1.1\r\nHost: api.example.com\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

	const post = serializeRequest("POST", new URL("https://example.com:8443/x"), {}, Buffer.from("あ")).toString("latin1");
	assert.ok(post.startsWith("POST /x HTTP/1.1\r\nHost: example.com:8443\r\nContent-Length: 3\r\n"));
	const empty = serializeRequest("PUT", new URL("http://[::1]:8080/"), {}, null).toString("latin1");
	assert.ok(empty.includes("Host: [::1]:8080\r\n") && empty.includes("Content-Length: 0\r\n"));
});

test("要求の組み立て: ヘッダの差し込み・予約ヘッダ・重複・ASCII 以外は拒否", () => {
	const url = new URL("https://example.com/");
	assert.throws(() => serializeRequest("GET", url, { "X-A": "a\r\nInjected: 1" }, null), /invalid value/);
	assert.throws(() => serializeRequest("GET", url, { "X A": "a" }, null), /invalid header name/);
	assert.throws(() => serializeRequest("GET", url, { "Host": "evil" }, null), /set by the transport/);
	assert.throws(() => serializeRequest("GET", url, { "Accept-Encoding": "gzip" }, null), /set by the transport/);
	assert.throws(() => serializeRequest("GET", url, { "Transfer-Encoding": "chunked" }, null), /set by the transport/);
	assert.throws(() => serializeRequest("GET", url, { "x-a": "1", "X-A": "2" }, null), /duplicate/);
	assert.throws(() => serializeRequest("GET", url, { "X-A": "日本語" }, null), /invalid value/);
	assert.throws(() => serializeRequest("DELETE" as any, url, {}, null), /unsupported method/);
});

test("応答: Content-Length と chunked (拡張・トレーラ付き) を読む", () => {
	const fixed = parseAll("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\":true}");
	assert.deepStrictEqual([fixed.status, fixed.body, fixed.ended, fixed.headers["content-type"]], [200, "{\"ok\":true}", true, "application/json"]);
	assert.strictEqual(Object.getPrototypeOf(fixed.headers), null);

	const chunked = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: t\r\n\r\n");
	assert.deepStrictEqual([chunked.body, chunked.ended], ["Wikipedia", true]);

	const merged = parseAll("HTTP/1.1 200 OK\r\nSet-X: a\r\nset-x: b\r\nContent-Length: 0\r\n\r\n");
	assert.strictEqual(merged.headers["set-x"], "a, b");
});

test("応答: どのバイト位置で分割されても同じ結果", () => {
	const raw = Buffer.from("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nこん\r\n6\r\nにち\r\n0\r\n\r\n", "utf8");
	const whole = parseAll(raw);
	assert.strictEqual(whole.body, "こんにち");
	for (let i = 1; i < raw.length; i++) assert.deepStrictEqual(parseAll(raw, LIMITS, [i]), whole, `split at ${i}`);
});

test("応答: 中間応答 (1xx) は読み捨て、101 は拒否。204 / 304 は本文なし", () => {
	assert.strictEqual(parseAll("HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok").body, "ok");
	rejects("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n", "PROTOCOL");
	assert.deepStrictEqual([parseAll("HTTP/1.1 204 No Content\r\n\r\n").ended, parseAll("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n").ended], [true, true]);
});

test("応答: 曖昧な形は拒否する", () => {
	rejects("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nContent-Length: 0x10\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\n\r\nbody until close", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 2\r\n\r\nxx", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 2\r\n\r\nxx", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nX-A: a\r\n folded\r\nContent-Length: 0\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nX-A : a\r\nContent-Length: 0\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nX-A: a\nb\r\nContent-Length: 0\r\n\r\n", "PROTOCOL");
	rejects("HTTP/2 200\r\nContent-Length: 0\r\n\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", "PROTOCOL");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n", "PROTOCOL");
	//同じ値の重複と identity は許す
	assert.strictEqual(parseAll("HTTP/1.1 200 OK\r\nContent-Length: 2, 2\r\nContent-Encoding: identity\r\n\r\nok").body, "ok");
});

test("応答: 途中で切れたら CLOSED", () => {
	rejects("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", "CLOSED");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", "CLOSED");
	rejects("HTTP/1.1 200 OK\r\nContent-Le", "CLOSED");
	rejects("", "CLOSED");
});

test("応答: ヘッダの上限ちょうどは受け、+1 は拒否", () => {
	const head = (pad: number) => `HTTP/1.1 200 OK\r\nX-Pad: ${"a".repeat(pad)}\r\nContent-Length: 0\r\n\r\n`;
	const exact = head(100).length;
	const limits = { ...LIMITS, maxHeaderBytes: exact };
	assert.strictEqual(parseAll(head(100), limits).ended, true);
	rejects(head(101), "LIMIT", limits);
	//終わりの来ないヘッダも上限で止める
	rejects("HTTP/1.1 200 OK\r\nX: " + "a".repeat(exact), "LIMIT", limits);
	const many = "HTTP/1.1 200 OK\r\n" + Array.from({ length: 4 }, (_, i) => `X-${i}: v\r\n`).join("") + "Content-Length: 0\r\n\r\n";
	assert.strictEqual(parseAll(many, { ...LIMITS, maxHeaderCount: 5 }).ended, true);
	rejects(many, "LIMIT", { ...LIMITS, maxHeaderCount: 4 });
});

test("応答: 本文の上限ちょうどは受け、+1 は拒否 (Content-Length・chunked とも)", () => {
	const limits = { ...LIMITS, maxBodyBytes: 10 };
	assert.strictEqual(parseAll("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", limits).body, "0123456789");
	rejects("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n", "LIMIT", limits);
	assert.strictEqual(parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n01234\r\n5\r\n56789\r\n0\r\n\r\n", limits).body, "0123456789");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n01234\r\n6\r\n", "LIMIT", limits);
	//chunk の大きさの行が長すぎる
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;" + "e".repeat(2000), "LIMIT");
	rejects("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n123456789\r\n", "PROTOCOL");
});
