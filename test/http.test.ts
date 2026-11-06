import { test } from "node:test"
import * as assert from "node:assert"
import { Router } from "../src/http/router.js"
import { mergeBody } from "../src/http/body.js"
import { RouteDef } from "../src/http/types.js"

const noop = () => null;

const routes: RouteDef[] = [
	{ method: "GET", path: "/stat", handler: "stat#check" },
	{ method: "GET", path: "/user/{id:number}", handler: "user#byId" },
	{ method: "GET", path: "/user/{hash}", handler: "user#byHash" },
	{ method: "GET", path: "/games/{id:number}", handler: noop },
	{ method: "POST", path: "/login", handler: "user#login" },
];
const router = new Router(routes);

test("数値変数と文字列変数を型で振り分ける", () => {
	const byId = router.match("GET", "/user/1030")!;
	assert.strictEqual(byId.def.handler, "user#byId");
	assert.strictEqual(byId.params.id, 1030);
	assert.strictEqual(byId.query.id, 1030);

	const byHash = router.match("GET", "/user/423c1220-c2cf")!;
	assert.strictEqual(byHash.def.handler, "user#byHash");
	assert.strictEqual(byHash.params.hash, "423c1220-c2cf");
});

test("数値指定の変数に数値以外は一致しない", () => {
	assert.strictEqual(router.match("GET", "/games/abc"), null);
});

test("末尾スラッシュとクエリを扱える", () => {
	const m = router.match("GET", "/stat/?a=1&b=%E3%81%82")!;
	assert.strictEqual(m.def.handler, "stat#check");
	assert.strictEqual(m.query.a, "1");
	assert.strictEqual(m.query.b, "あ");
});

test("表に無いパス・メソッドは null (親ルートへフォールバックしない)", () => {
	assert.strictEqual(router.match("GET", "/stat/unknown"), null);
	assert.strictEqual(router.match("POST", "/stat"), null);
	assert.strictEqual(router.match("GET", "/../user/1"), null);
	assert.strictEqual(router.match("GET", "/user/%E0%A4%A"), null);
});

test("モジュール名の一覧は文字列ハンドラだけから作る", () => {
	assert.deepStrictEqual(router.modules().sort(), ["stat", "user"]);
});

test("不正なハンドラ名はルータ生成時に弾く", () => {
	assert.throws(() => new Router([{ method: "GET", path: "/x", handler: "../x#run" }]));
	assert.throws(() => new Router([{ method: "GET", path: "/x", handler: "x" }]));
});

test("ボディの __proto__ でプロトタイプを汚染できない", () => {
	const query: any = Object.create(null);
	mergeBody(query, '{"__proto__":{"polluted":1},"constructor":1,"a":2}');
	assert.strictEqual(query.a, 2);
	assert.strictEqual(Object.getPrototypeOf(query), null);
	assert.strictEqual(query.constructor, undefined);
	assert.strictEqual(({} as any).polluted, undefined);

	const query2: any = Object.create(null);
	mergeBody(query2, "__proto__=1&x=%E3%81%82");
	assert.strictEqual(query2.x, "あ");
	assert.strictEqual(Object.getPrototypeOf(query2), null);
});

test("クエリ文字列の __proto__ も無視する", () => {
	const m = router.match("GET", "/stat?__proto__=x&ok=1")!;
	assert.strictEqual(Object.getPrototypeOf(m.query), null);
	assert.strictEqual(m.query.ok, "1");
});
