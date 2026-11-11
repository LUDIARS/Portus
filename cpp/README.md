# Portus C++

Portus の C++17 実装。TypeScript 版 (`../src`) と同じ決め事 ([DESIGN.md](../DESIGN.md)) を、Node を載せずに動く静的ライブラリ `libportus` として提供する。

- **目的 1: メモリ。** 常駐するローカルサービス (Memoria など) 1 つごとに V8 を抱えない。待機時 約 6MB (Node 版は約 62MB、下の計測)
- **目的 2: サプライチェーン。** `npm install` で実行時の依存ツリーを取り込まない。外部依存は OS / パッケージマネージャが配る OpenSSL だけで、ビルド時に何も取得しない
- 高度な実装はしない。入出力は `poll` の単一イベントループ、ブロックする処理はワーカースレッド、という単純な形に留める

TypeScript 版を点検して見つけた点と、C++ 版での扱いは [docs/cpp-review.md](../docs/cpp-review.md)。

## 構成

```
イベントループ (1 スレッド, poll)          ワーカースレッド (既定 4)
  接続の受け入れ / 読み書き / タイマー        ハンドラ + ミドルウェア (ブロックしてよい)
  HTTP の解析・ルーティング  ── 要求 ──▶     Redis / fetch / S3 / Push の呼び出し
  WebSocket のフレーム処理   ◀── 応答 ──
  WebSocket のコールバック                  ログ書き込みスレッド (1)
```

- 遅いハンドラがあっても、イベントループは止まらない (他の要求・WebSocket はすぐ応える)
- イベント駆動は踏襲: WebSocket は `on_message` などのコールバック、HTTP は表に載せたハンドラ
- `worker_threads = 0` にすると、ハンドラをイベントループ上で直接動かす (ブロックしないハンドラだけの構成向け)

| ヘッダ | 中身 | TypeScript 版 |
|---|---|---|
| `portus/http.hpp` | 表駆動ルータ、HTTP/1.1 サーバ、ミドルウェア、ボディ上限 | `http` |
| `portus/ws.hpp` | WebSocket サーバ / クライアント (RFC 6455) | `ws` |
| `portus/push.hpp` | Web Push 送信 (VAPID + aes128gcm) | `push` |
| `portus/aws.hpp` | SigV4 署名、S3 PutObject、認証情報チェーン | `aws` |
| `portus/redis.hpp` | Redis クライアント (RESP2) | `redis` |
| `portus/ai.hpp` | OpenAI / Anthropic / Gemini / Notion の薄いクライアント | `ai` |
| `portus/net.hpp` | タイムアウト・リダイレクト禁止・証明書検証付きの HTTP(S) クライアント | `net` |
| `portus/log.hpp` | 構造化ロガー (レベル / JSON / 日別ファイル / 秘匿キーのマスク) | `log` |
| `portus/env.hpp` `id.hpp` `json.hpp` `crypto.hpp` | `.env`、UUID v4、JSON、OpenSSL の薄い包み | `env` `id` |
| `portus/event_loop.hpp` `thread_pool.hpp` `tcp_conn.hpp` | イベントループ、ワーカー、非ブロッキング接続 | (Node 本体) |

ホットリロード (`hot` / `portus-dev`) は移していない。ESM の読み直しに依存した開発専用の機能で、C++ では再ビルドして再起動する。ルート表だけは `HttpServer::set_routes` で止めずに差し替えられる。

## ビルド

要件: C++17 コンパイラ、CMake 3.16 以上、OpenSSL 3.x の開発パッケージ。

```
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure    # 外部サービス不要
```

サービス側からは `add_subdirectory(<portus>/cpp)` して `target_link_libraries(app PRIVATE portus::portus)`。

| 環境 | 状態 |
|---|---|
| Linux (gcc 13 / clang 18) | テスト全通過。ASan + UBSan、TSan でも通過 |
| Windows (Winsock) | コードは `#ifdef _WIN32` で対応済み。MinGW-w64 で OpenSSL に依存しないファイルの構文・型検査のみ実施。**実機でのビルドと実行は未確認** |
| macOS | 未確認 (POSIX 側のコードを使う) |

## HTTP

```cpp
#include "portus/http.hpp"
using namespace portus;

EventLoop loop;

HttpServerOptions options;
options.log = logger_from_env("app").child("http");
options.allowed_hosts = { "127.0.0.1", "localhost" };   // DNS リバインディング対策 (下記)

// "モジュール#関数" で参照するハンドラ。表に載せた名前だけが呼べる
options.handlers["user#byId"] = [](RequestContext& ctx) -> Reply {
	return Json::object({ { "id", ctx.params.at("id") } });
};

// 表に書いたものだけが到達可能。上から順に評価し、最初に一致したものを使う
options.routes = {
	{ "GET",  "/user/{id:number}", "user#byId" },
	{ "POST", "/login", [](RequestContext& ctx) -> Reply { return ctx.query; }, Json::object({ { "public", true } }) },
	{ "GET",  "/health", [](RequestContext&) -> Reply { return "ok"; }, Json::object({ { "public", true } }) },
};

// 認証などはミドルウェアで足す。next() を呼ばずに返せば打ち切り
options.middlewares.push_back([](RequestContext& ctx, const Next& next) -> Reply {
	if (!ctx.route->meta.at("public").as_bool() && !ctx.headers.find("authorization")) return text("unauthorized", 401);
	return next();
});

HttpServer http(loop, std::move(options));
http.listen(8080);   // 既定の待ち受けは 127.0.0.1
loop.run();
```

ハンドラは `Reply(RequestContext&)`。`ctx.query` はクエリ・ボディ (JSON オブジェクト / フォーム)・パス変数をまとめたもので、`ctx.params` はパス変数だけ。

| 戻り値 | 応答 |
|---|---|
| `json(data, status)` / `text(...)` / `html(...)` / `Reply(status, body, headers)` | そのまま |
| `Reply::no_content()` | 204 |
| 文字列 | 200 text/plain |
| `Json` | 200 JSON |
| `Reply::bytes(data)` | 200 application/octet-stream |

既定の安全動作:

| 項目 | 既定 |
|---|---|
| 待ち受け | `127.0.0.1` |
| ボディ | 1MiB、超過 413。`Transfer-Encoding` 付きの要求は 411 (長さの解釈違いによる要求の紛れ込みを作らない) |
| ヘッダ | 16KiB / 100 行、超過 431。名前の後ろの空白・行の折り返し・食い違う `Content-Length` は 400 |
| 時間 | 要求を受け切るまで 30 秒、応答後の待機 5 秒 |
| 同時接続 | 1024。ワーカー待ち 1024 件を超えたら 503 |
| 応答ヘッダ | 改行などを含む値は送らず 500 (応答の分割を防ぐ) |
| ハンドラの例外 | 500 `internal error.` + エラーログ。全応答に `x-request-id` |
| キー | `__proto__` `constructor` `prototype` は取り込まない (応答を受ける JS 側の保護) |

`allowed_hosts` と WebSocket の `allowed_origins` は既定では空 (検査なし) で、TypeScript 版と同じ動きになる。**ブラウザのある PC で動かすローカルサービスでは両方を設定する。** 設定しないと、悪意のあるページから `127.0.0.1` のサービスを呼べる (DNS リバインディング / Cross-Site WebSocket Hijacking)。

## WebSocket

```cpp
#include "portus/ws.hpp"

WebSocketServerOptions ws_options;
ws_options.max_payload = 64 * 1024;
ws_options.allowed_origins = { "http://localhost:8080" };
WebSocketServer wss(loop, ws_options);
wss.on_connection = [](const std::shared_ptr<WebSocket>& ws, const RequestHead& request) {
	std::weak_ptr<WebSocket> weak = ws;
	ws->on_message = [weak](std::string_view data, bool is_binary) {
		if (auto s = weak.lock()) s->send("echo:" + std::string(data));
	};
	ws->on_close = [](int code, std::string_view reason) {};
};
wss.listen(3000);

auto client = connect_websocket(loop, "ws://127.0.0.1:3000/");
client->on_open = [weak = std::weak_ptr<WebSocket>(client)] { if (auto c = weak.lock()) c->send("hello"); };
```

- `send` / `send_binary` / `ping` / `close` / `terminate` / `ready_state` と、`on_open` / `on_message` / `on_ping` / `on_pong` / `on_close` / `on_error`
- コールバックはイベントループのスレッドで呼ばれる。中でブロックしない (重い処理は `ThreadPool` へ)。`send` などはどのスレッドからでも呼べる
- ペイロード 1MiB 超過は 1009、マスク無し・RSV・不正な UTF-8・不正な close フレームは切断
- 送信待ちが 16MiB を超えた接続は切る (読まない相手にメモリを取られない)
- クライアントは `ws://` だけ。`wss://` は持たない (TLS はリバースプロキシで終端する)

## 外向きの通信・Redis・ログ

```cpp
#include "portus/net.hpp"     // どれも完了まで戻らない。ハンドラ (ワーカースレッド) から呼ぶ
JsonRequest req;
req.headers = { { "Authorization", "Bearer ..." } };
req.body = Json::object({ { "q", "x" } });
Json res = fetch_json("https://api.example.com/v1/items", req);

#include "portus/redis.hpp"
RedisClient redis("127.0.0.1", 6379);
redis.set("k", "v");

#include "portus/push.hpp"
send_notification(subscription, "{\"title\":\"通知\"}", { "mailto:admin@example.com", VAPID_PUBLIC, VAPID_PRIVATE });

#include "portus/aws.hpp"
put_object("ap-northeast-1", "my-bucket", "logs/a.txt", "body");   // 認証: 環境変数 → ~/.aws/credentials → IMDSv2
```

- `fetch` はリダイレクトに従わない、合計時間で打ち切る、応答は 16MiB まで、https は証明書の連鎖とホスト名を検証する (`ca_file` で CA を指定可)
- `HTTPS_PROXY` などのプロキシ設定は見ない。名前解決 (`getaddrinfo`) には個別のタイムアウトが無い
- ログは `logger_from_env(scope)` (`LOG_LEVEL` / `LOG_FORMAT` / `LOG_DIR`)。書き込みは専用スレッドで行い、詰まったら溜め込まずに捨てて件数を残す
- ライブラリ初期化時に `SIGPIPE` を無視に設定する (POSIX)。切れた相手への書き込みをエラーとして受け取るため

## 計測

`examples/hello_server.cpp` と、同じルート・ミドルウェアを TypeScript 版で書いたものを比べた。
条件: Linux x86-64 (4 コア)、Node.js 22.23、gcc 13 `-O2`、ログは標準出力 (pretty)、負荷は同じ PC 上の Node 製クライアント (keep-alive 100 接続、`GET /health` と 1KB の `POST /echo` を交互)。

| | C++ (ワーカー 4) | C++ (`worker_threads = 0`) | Node (TypeScript 版) |
|---|---|---|---|
| 待機時の RSS | 5.8 MB | 5.8 MB | 62.4 MB |
| 8 万件処理後の RSS | 9.4 MB | 8.8 MB | 81.9 MB |
| 接続 1000 本を保持中の RSS | 10.0 MB | — | 87.5 MB |
| 応答時間 p50 / p99 | 3.0 ms / 14.4 ms | 4.0 ms / 14.0 ms | 3.6 ms / 13.4 ms |
| サーバの CPU 時間 (6 万件) | 2.7 秒 | 2.2 秒 | 2.3 秒 |

- メモリは約 1/9。CPU 時間と応答時間は Node 版と同程度 (この計測はクライアント側が上限になっており、サーバ単体の最大性能を測ったものではない)
- 1 回の計測値で、環境によって変わる。傾向の確認用
