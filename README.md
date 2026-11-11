# Portus

> ラテン語で「港」。LUDIARS の **Node サーバ基盤ライブラリ**。

脆弱性報告がよく上がる npm 依存 (`ws` / `web-push` / `aws-sdk` / `redis` / 各 AI SDK / Web フレームワーク) を使わず、
サーバに必要な部品を **使う機能だけ・実行時依存ゼロ** で提供する。

- 通信は Node 標準 (`http` / `net` / `fetch`)、暗号は `node:crypto` だけを使う
- ルーティングは「表に書いたものだけが到達可能」な透明な表駆動
- 構造化ログとホットリロードを最初から持つ

どの依存をなぜ置き換えたかは [docs/dependency-audit.md](docs/dependency-audit.md)、設計の決め事は [DESIGN.md](DESIGN.md)。

## C++ 版

同じ決め事を Node 無しで動く C++17 のライブラリにしたものが [`cpp/`](cpp/README.md) にある。
常駐するローカルサービスのメモリを減らし (待機時 約 6MB、Node 版は約 62MB)、`npm install` による実行時依存の取り込みを無くすためのもの。
TypeScript 版を点検して見つけた点は [docs/cpp-review.md](docs/cpp-review.md)。以下は TypeScript 版の説明。

## モジュール

| import | 中身 | 置き換え対象 |
|---|---|---|
| `@ludiars/portus/http` | 表駆動ルータ、HTTP サーバ、ミドルウェア、ボディ上限 | Hono / Express などのフレームワーク |
| `@ludiars/portus/ws` | WebSocket サーバ / クライアント (RFC 6455) | `ws` |
| `@ludiars/portus/push` | Web Push 送信 (VAPID + aes128gcm) | `web-push` |
| `@ludiars/portus/aws` | SigV4 署名、S3 PutObject、認証情報チェーン | `aws-sdk` v2 |
| `@ludiars/portus/redis` | Redis クライアント (GET / SET / HSET / HGETALL / 任意コマンド) | `redis` |
| `@ludiars/portus/ai` | OpenAI / Anthropic / Gemini / Notion の薄いクライアント | 各 SDK |
| `@ludiars/portus/net` | タイムアウト・リダイレクト禁止付き JSON fetch | `node-fetch` / `axios` |
| `@ludiars/portus/log` | 構造化ロガー (レベル / JSON / 日別ファイル / 秘匿キーのマスク) | `pino` / `winston` |
| `@ludiars/portus/hot` | ハンドラとルート表のホットリロード | `nodemon` / `ts-node-dev` |
| `@ludiars/portus/env` | `.env` 読み込み | `dotenv` |
| `@ludiars/portus/id` | UUID v4 | `uuid` |

## HTTP

```ts
import { createHttpServer, json, text, RouteDef, Middleware } from "@ludiars/portus/http"
import { loggerFromEnv, installConsoleBridge, installProcessHandlers } from "@ludiars/portus/log"
import { loadEnv } from "@ludiars/portus/env"
import { fileURLToPath } from "node:url"

loadEnv();
const log = loggerFromEnv("app");
installConsoleBridge(log.child("console"));
installProcessHandlers(log);

//表に書いたものだけが到達可能。上から順に評価し、最初に一致したものを使う
export const routes: RouteDef[] = [
	{ method: "GET",  path: "/user/{id:number}", handler: "user#byId" },	//handlers/user.js の byId
	{ method: "GET",  path: "/user/{hash}",      handler: "user#byHash" },
	{ method: "POST", path: "/login",            handler: "user#login", meta: { public: true } },
	{ method: "GET",  path: "/health",           handler: () => "ok" },	//関数も直接書ける
];

//認証などはミドルウェアで足す。next() を呼ばずに返せば打ち切り
const auth: Middleware = async (ctx, next) => {
	if (!ctx.route.meta?.public && !ctx.req.headers.authorization) return text("unauthorized", 401);
	return next();
};

const app = createHttpServer({
	routes,
	handlersDir: fileURLToPath(new URL("./handlers", import.meta.url)),
	middlewares: [auth],
	log: log.child("http"),
	hotReload: process.env.PORTUS_HOT == "1",
	routesFile: fileURLToPath(new URL("./routes.js", import.meta.url)),
});
await app.listen(8080);	//既定の待ち受けは 127.0.0.1
```

ハンドラは `(ctx) => 戻り値`。`ctx.query` はクエリ・パス変数・ボディ (JSON / フォーム) をまとめたもので、`ctx.params` はパス変数だけ。

| 戻り値 | 応答 |
|---|---|
| `json(data, status)` / `text(...)` / `html(...)` / `new Reply(...)` | そのまま |
| `undefined` / `null` | 204 |
| 文字列 | 200 text/plain |
| `Buffer` | 200 application/octet-stream |
| その他のオブジェクト | 200 JSON |

既定の安全動作: ボディ 1MiB 上限 (413)、`__proto__` などのキーは取り込まない、ハンドラ例外は 500 + エラーログ、全応答に `x-request-id`。

## WebSocket

```ts
import { WebSocketServer, connectWebSocket } from "@ludiars/portus/ws"

const wss = new WebSocketServer({ port: 3000, maxPayload: 64 * 1024 });
wss.on("connection", (ws, req) => {
	ws.on("message", (data, isBinary) => ws.send(data.toString()));
	ws.on("close", (code) => {});
});

const client = connectWebSocket("ws://127.0.0.1:3000/");
client.on("open", () => client.send("hello"));
```

`send` / `ping` / `close` / `terminate` / `readyState` と、`open` / `message` / `ping` / `pong` / `close` / `error` イベントを持つ (`ws` で一般的に使う範囲)。拡張 (permessage-deflate など) は持たない。

## Web Push

```ts
import { sendNotification } from "@ludiars/portus/push"

await sendNotification(subscription, JSON.stringify({ title: "通知" }), {
	subject: "mailto:admin@example.com", publicKey: VAPID_PUBLIC, privateKey: VAPID_PRIVATE,
});
```

送信先は既知の push サービス (FCM / Mozilla / Apple / Windows) の https だけ。`{ allowedHosts }` で変えられる。

## S3 / Redis / AI

```ts
import { putObject } from "@ludiars/portus/aws"
await putObject("ap-northeast-1", "my-bucket", "logs/a.txt", "body");	//認証: 環境変数 → ~/.aws/credentials → IMDSv2

import { RedisClient } from "@ludiars/portus/redis"
const redis = new RedisClient("127.0.0.1", 6379);
await redis.set("k", "v");

import { OpenAIClient, AnthropicClient, GeminiClient } from "@ludiars/portus/ai"
const msg = await new AnthropicClient(KEY).createMessage({ model: "claude-sonnet-5-5", max_tokens: 1000, messages: [{ role: "user", content: "hi" }] });
```

## ログ

`loggerFromEnv(scope)` は次の環境変数を見る。

| 環境変数 | 値 | 既定 |
|---|---|---|
| `LOG_LEVEL` | `debug` / `info` / `warn` / `error` | `info` |
| `LOG_FORMAT` | `pretty` / `json` (JSON Lines) | `pretty` |
| `LOG_DIR` | 指定するとそのディレクトリに `<scope>-YYYY-MM-DD.log` も書く | なし |

- `installConsoleBridge(log)` で既存コードの `console.*` も同じ形式・レベルに揃う
- HTTP は 1 リクエスト 1 行のアクセスログ (`reqId` / `handler` / `status` / `ms`)。ボディとクエリは出さない
- フィールドの `session` `token` `password` `authorization` などは `[REDACTED]`

## ホットリロード (開発)

```
npx portus-dev --entry dist/main.js --out dist --hot "handlers/*.js,routes.js" -- --port 8080
```

- `tsc --watch` とアプリを一緒に起動し、アプリには `PORTUS_HOT=1` を渡す
- `--hot` に一致するファイル (ハンドラ・ルート表) は、アプリを止めずに次のリクエストから差し替わる (WebSocket 接続は切れない)
- それ以外が変わったら、コンパイルが通った後にアプリを再起動する。コンパイルエラーの間は古いコードのまま動く
- ESM はモジュールキャッシュを消せないため、版番号付きで読み直す。古い版はメモリに残るので本番では使わない

## 開発

```
npm install
npm test        # node:test。外部サービス不要
npm run build
```

要件: Node.js 22.12 以上。
