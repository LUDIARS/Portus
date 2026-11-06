import { test } from "node:test"
import * as assert from "node:assert"
import * as fs from "node:fs"
import * as os from "node:os"
import * as path from "node:path"
import { createHttpServer, json, text, Middleware, RouteDef } from "../src/http/index.js"
import { createRootLogger, LogRecord } from "../src/log/index.js"

function memoryLogger() {
	const records: LogRecord[] = [];
	return { records, log: createRootLogger("test", "debug", [{ write: (r) => records.push(r) }]) };
}

function waitUntil(cond: () => Promise<boolean>, timeoutMs: number): Promise<void> {
	return new Promise((resolve, reject) => {
		const started = Date.now();
		const tick = async () => {
			if (await cond()) return resolve();
			if (Date.now() - started > timeoutMs) return reject(new Error("timeout"));
			setTimeout(tick, 30);
		};
		void tick();
	});
}

test("関数ハンドラ・ボディ結合・応答の型変換・アクセスログ", async () => {
	const { records, log } = memoryLogger();
	const routes: RouteDef[] = [
		{ method: "GET", path: "/hello/{name}", handler: (ctx) => `hello ${ctx.params.name}` },
		{ method: "POST", path: "/echo", handler: (ctx) => ({ got: ctx.query.a, session: ctx.query.session }) },
		{ method: "GET", path: "/empty", handler: () => undefined },
		{ method: "GET", path: "/teapot", handler: () => json({ short: "stout" }, 418) },
		{ method: "GET", path: "/boom", handler: () => { throw new Error("boom"); } },
	];
	const app = createHttpServer({ routes, log, maxBodyBytes: 1024 });
	const { port } = await app.listen(0);
	const base = `http://127.0.0.1:${port}`;

	try {
		const r1 = await fetch(`${base}/hello/%E4%B8%96%E7%95%8C`);
		assert.strictEqual(r1.status, 200);
		assert.strictEqual(await r1.text(), "hello 世界");
		assert.match(r1.headers.get("x-request-id") || "", /^[0-9a-f-]{36}$/);

		const r2 = await fetch(`${base}/echo`, { method: "POST", body: JSON.stringify({ a: 1, session: "secret-session" }) });
		assert.deepStrictEqual(await r2.json(), { got: 1, session: "secret-session" });

		const r3 = await fetch(`${base}/echo`, { method: "POST", body: "a=x&b=y" });
		assert.deepStrictEqual(await r3.json(), { got: "x" });

		assert.strictEqual((await fetch(`${base}/empty`)).status, 204);
		assert.strictEqual((await fetch(`${base}/teapot`)).status, 418);
		assert.strictEqual((await fetch(`${base}/nothing`)).status, 404);
		assert.strictEqual((await fetch(`${base}/echo`, { method: "POST", body: "x".repeat(4096) })).status, 413);

		const r4 = await fetch(`${base}/boom`);
		assert.strictEqual(r4.status, 500);
		assert.strictEqual(await r4.text(), "internal error.");
	} finally {
		await app.close();
	}

	const access = records.filter((r) => r.msg == "access");
	assert.strictEqual(access.length, 8);
	assert.ok(access.some((r) => r.fields.path == "/hello/%E4%B8%96%E7%95%8C" && r.fields.status == 200));
	assert.ok(records.some((r) => r.level == "error" && r.msg == "handler failed"));
	//ログにボディ・クエリは出さない
	assert.ok(!JSON.stringify(records).includes("secret-session"));
});

test("ミドルウェアで打ち切り・前後処理ができる", async () => {
	const auth: Middleware = async (ctx, next) => {
		if (ctx.route.meta?.auth && ctx.req.headers["x-token"] != "ok") return text("unauthorized", 401);
		ctx.state.user = "alice";
		const result: any = await next();
		return { ...result, wrapped: true };
	};
	const routes: RouteDef[] = [
		{ method: "GET", path: "/me", handler: (ctx) => ({ user: ctx.state.user }), meta: { auth: true } },
	];
	const app = createHttpServer({ routes, middlewares: [auth] });
	const { port } = await app.listen(0);
	try {
		assert.strictEqual((await fetch(`http://127.0.0.1:${port}/me`)).status, 401);
		const ok = await fetch(`http://127.0.0.1:${port}/me`, { headers: { "x-token": "ok" } });
		assert.deepStrictEqual(await ok.json(), { user: "alice", wrapped: true });
	} finally {
		await app.close();
	}
});

test("モジュール#関数 ハンドラとルート表をホットリロードできる", async () => {
	const dir = fs.mkdtempSync(path.join(os.tmpdir(), "portus-hot-"));
	const handlersDir = path.join(dir, "handlers");
	fs.mkdirSync(handlersDir);
	fs.writeFileSync(path.join(handlersDir, "greet.js"), "export function hello() { return 'v1'; }\n");
	const routesFile = path.join(dir, "routes.js");
	fs.writeFileSync(routesFile, "export const routes = [{ method: 'GET', path: '/hello', handler: 'greet#hello' }];\n");

	const { routes } = await import(new URL(`file:///${routesFile.replace(/\\/g, "/")}`).href);
	const app = createHttpServer({ routes, handlersDir, hotReload: true, routesFile });
	const { port } = await app.listen(0);
	const get = async (p: string) => {
		const r = await fetch(`http://127.0.0.1:${port}${p}`);
		return `${r.status}:${await r.text()}`;
	};

	try {
		assert.strictEqual(await get("/hello"), "200:v1");

		//ハンドラだけ書き換え → サーバを止めずに新しい実装
		fs.writeFileSync(path.join(handlersDir, "greet.js"), "export function hello() { return 'v2'; }\nexport function bye() { return 'bye'; }\n");
		await waitUntil(async () => (await get("/hello")) == "200:v2", 3000);

		//ルート表を書き換え → 新しいルートが有効になる
		fs.writeFileSync(routesFile, "export const routes = [{ method: 'GET', path: '/hello', handler: 'greet#hello' }, { method: 'GET', path: '/bye', handler: 'greet#bye' }];\n");
		await waitUntil(async () => (await get("/bye")) == "200:bye", 3000);

		//壊れたルート表は採用しない
		fs.writeFileSync(routesFile, "export const routes = [{ method: 'GET', path: '/x', handler: '../evil#run' }];\n");
		await new Promise((r) => setTimeout(r, 500));
		assert.strictEqual(await get("/bye"), "200:bye");
		assert.strictEqual(app.routes().length, 2);
	} finally {
		await app.close();
	}
});
