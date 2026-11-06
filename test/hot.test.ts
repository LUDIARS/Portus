import { test } from "node:test"
import * as assert from "node:assert"
import * as fs from "node:fs"
import * as os from "node:os"
import * as path from "node:path"
import { HandlerRegistry } from "../src/hot/handlerRegistry.js"

test("登録外のモジュール名・パス区切りを含む名前は読まない", async () => {
	const dir = fs.mkdtempSync(path.join(os.tmpdir(), "portus-reg-"));
	fs.writeFileSync(path.join(dir, "secret.js"), "export const run = () => 1;\n");
	const reg = new HandlerRegistry(dir, ["vc"]);
	assert.strictEqual(await reg.load("secret"), null);
	assert.strictEqual(await reg.load("../secret"), null);
});

test("ホットリロード無効時は書き換えても同じ版を使う", async () => {
	const dir = fs.mkdtempSync(path.join(os.tmpdir(), "portus-reg-"));
	const file = path.join(dir, "vc.js");
	fs.writeFileSync(file, "export const run = () => 'v1';\n");
	const reg = new HandlerRegistry(dir, ["vc"]);
	assert.strictEqual((await reg.load("vc")).run(), "v1");

	fs.writeFileSync(file, "export const run = () => 'v2';\n");
	await new Promise((r) => setTimeout(r, 300));
	assert.strictEqual((await reg.load("vc")).run(), "v1");
});
