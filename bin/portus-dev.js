#!/usr/bin/env node
//開発用ランチャー: tsc --watch とアプリを一緒に動かす
//  - --hot に一致するファイル (ハンドラ・ルート表) の変更は、アプリ側のホットリロードに任せて再起動しない
//  - それ以外の .js が変わったら、コンパイル成功後にアプリを再起動する
//  - コンパイルエラーの間は再起動しない (古いコードのまま動き続ける)
//  - アプリには PORTUS_HOT=1 を渡す (createHttpServer の hotReload を有効にする目印)
//
//使い方:
//  portus-dev --entry dist/main.js --out dist --hot "handlers/*.js,routes.js" [--project tsconfig.json] [-- アプリ引数...]
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import * as fs from "node:fs";
import * as path from "node:path";

function parseArgs(argv) {
	const opts = { entry: "dist/main.js", out: "dist", hot: "", project: "tsconfig.json", rest: [] };
	for (let i = 0; i < argv.length; ++i) {
		const a = argv[i];
		if (a == "--") {
			opts.rest = argv.slice(i + 1);
			break;
		}
		const key = a.replace(/^--/, "");
		if (!(key in opts) || key == "rest") throw new Error(`unknown option ${a}`);
		opts[key] = argv[++i];
	}
	return opts;
}

//"handlers/*.js" のような単純なパターン (* は区切りを越えない) を正規表現にする
function patternToRegExp(pattern) {
	const escaped = pattern.trim().replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*");
	return new RegExp(`^${escaped}$`);
}

const opts = parseArgs(process.argv.slice(2));
const root = process.cwd();
const outDir = path.resolve(root, opts.out);
const hotPatterns = opts.hot.split(",").filter((p) => p.trim() != "").map(patternToRegExp);
const tscBin = createRequire(path.join(root, "package.json")).resolve("typescript/bin/tsc");

const isHotSwappable = (rel) => hotPatterns.some((re) => re.test(rel.split(path.sep).join("/")));

let app = null;
let restarting = false;
const changed = new Set();
//tsc は依存元のファイルも中身が同じまま書き直すため、中身のハッシュで本当に変わったものだけを見る
const hashes = new Map();

function contentChanged(rel) {
	let hash;
	try {
		hash = createHash("sha1").update(fs.readFileSync(path.join(outDir, rel))).digest("hex");
	} catch (ex) {
		hash = "";
	}
	const prev = hashes.get(rel);
	hashes.set(rel, hash);
	return prev !== hash;
}

//起動時点の出力を基準として記録する
function seedHashes() {
	for (const rel of fs.readdirSync(outDir, { recursive: true })) {
		if (String(rel).endsWith(".js")) contentChanged(String(rel));
	}
}

function log(msg) {
	process.stdout.write(`${new Date().toISOString()} DEV   [portus-dev] ${msg}\n`);
}

function startApp() {
	app = spawn(process.execPath, ["--enable-source-maps", path.resolve(root, opts.entry), ...opts.rest], {
		cwd: root,
		stdio: "inherit",
		env: { ...process.env, PORTUS_HOT: "1" },
	});
	app.on("exit", (code, signal) => {
		app = null;
		if (!restarting) log(`app exited code=${code} signal=${signal}`);
	});
}

function restartApp() {
	if (!app) return startApp();
	restarting = true;
	app.once("exit", () => {
		restarting = false;
		startApp();
	});
	app.kill();
}

//コンパイル 1 回分の変更をまとめて判断する
function onCompiled(errors) {
	if (errors > 0) {
		//変更は持ち越し、エラーが解消したコンパイルでまとめて判断する
		log(`compile errors=${errors}, app keeps running old code`);
		return;
	}
	if (!app && !restarting) {
		changed.clear();
		seedHashes();
		log("starting app");
		return startApp();
	}
	const files = Array.from(changed).filter(contentChanged);
	changed.clear();
	const needRestart = files.filter((f) => f.endsWith(".js") && !isHotSwappable(f));
	if (needRestart.length > 0) {
		log(`restart (${needRestart.slice(0, 3).join(", ")}${needRestart.length > 3 ? ", ..." : ""})`);
		restartApp();
	} else if (files.length > 0) {
		log(`hot swap only (${files.length} files)`);
	}
}

fs.mkdirSync(outDir, { recursive: true });
fs.watch(outDir, { recursive: true }, (event, name) => {
	if (name) changed.add(name.toString());
});

const tsc = spawn(process.execPath, [tscBin, "--watch", "--preserveWatchOutput", "-p", opts.project], { cwd: root, stdio: ["ignore", "pipe", "inherit"] });
tsc.stdout.setEncoding("utf8");
let pending = "";
tsc.stdout.on("data", (chunk) => {
	process.stdout.write(chunk);
	pending += chunk;
	const lines = pending.split(/\r?\n/);
	pending = lines.pop();
	for (const line of lines) {
		const m = /Found (\d+) errors?\. Watching for file changes/.exec(line);
		//fs.watch の通知が揃うのを少し待つ
		if (m) setTimeout(() => onCompiled(Number(m[1])), 200);
	}
});

function shutdown() {
	tsc.kill();
	if (app) app.kill();
	process.exit(0);
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
