# C++ の HTTP・WebSocket 共有待ち受け

関連タスク `actio:489c077d-3b24-431d-862d-dca08b15fdca`

`portus/http_websocket.hpp` の `WebSocketUpgradeOptions` を `portus/http.hpp` の `HttpServerOptions::websocket_upgrade` に設定する。`HttpServer::listen` で HTTP ルートと WebSocket を同じアドレス、ポートで受け付ける。設定を省略すると HTTP ルートだけを扱う。独立待ち受けの `WebSocketServer` も利用できる。

- 設定した生のパスに完全一致し、`Upgrade` フィールドに `websocket` トークンがある要求を対象にする。クエリはパスの比較から外し、`RequestHead::target` に保持する。通常の HTTP 要求は既存のルート表で解決する。
- HTTP のヘッダー、Host、本文長の検査を先に適用する。続いて本文のない HTTP/1.1 の WebSocket ハンドシェイクを検証する。認可は `authorize` に `RequestHead` と接続元アドレスを渡し、`std::nullopt` は 403、例外は 500 とする。認可と接続受け入れのコールバックは設定必須で、イベントループ上で同期的に実行する。トークン検証などはイベントループをブロックせずに完結する処理を使う。
- 認可が成立すると HTTP の管理表から外し、同じ TCP 接続と受信済みバイトを WebSocket に渡す。`101` を送信待ちに入れた後、`on_connection` に認可結果を渡してから先読みフレームを処理する。要求・認可結果はコールバック中に借用し、後で必要な値はコピーする。
- HTTP と WebSocket の同時接続数の合計を `HttpServerOptions::max_connections` で制限する。切り替え前は HTTP のヘッダー、本文、要求タイムアウトの設定を適用し、切り替え後は `max_payload` と `max_buffered` を適用する。`HttpServer::close()` は両方の接続を閉じる。

## Actio の組み込み例

`token_verifier` は `RequestHead::target` の `token` クエリを読み、Actio の既存のトークン検証処理でユーザー ID を解決する。`allowed_origin` は Actio が許可するブラウザの Origin。検証済みのユーザー ID を購読や配信の権限判定にも使う。

```cpp
#include "portus/http.hpp"
#include "portus/http_websocket.hpp"

portus::WebSocketUpgradeOptions upgrade;
upgrade.path = "/ws";
upgrade.authorize = [&](const portus::RequestHead& request, std::string_view) -> std::optional<portus::Json> {
    if (request.headers.get("origin") != allowed_origin) return std::nullopt;
    const auto user_id = token_verifier(request);
    if (!user_id) return std::nullopt;
    return portus::Json::object({{"userId", *user_id}});
};
upgrade.on_connection = [&](const std::shared_ptr<portus::WebSocket>& ws,
                            const portus::RequestHead&, const portus::Json& auth) {
    subscriptions.attach(ws, auth.at("userId").as_string());
};
http_options.websocket_upgrade = std::move(upgrade);
portus::HttpServer http(loop, std::move(http_options));
http.listen(17880);
```

ビルド可能なローカル用の例は `cpp/examples/hello_server.cpp`。回帰テストは `cpp/tests/http_websocket_test.cpp` で同一接続上の HTTP 応答からの切り替え、認可失敗、先読み、入力と接続数の上限、切断時の参照解放を確認する。
