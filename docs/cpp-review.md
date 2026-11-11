# C++ 化にあたっての点検結果

C++ 版 (`cpp/`) を作る前に TypeScript 版 (`src/`) を読み、移すときに直すべき点を洗い出した。
「確認」は、ビルドした TypeScript 版に実際の入力を与えて挙動を見たもの。

**TypeScript 版はこの変更では直していない。** 下の表の「TypeScript 版」は現状の挙動で、修正は別の変更で行う。

## 1. 見つけた点

| # | 場所 | 内容 | 確認 | C++ 版 |
|---|---|---|---|---|
| 1 | `ws/socket.ts` `onFrame` | 長さ 0 の継続フレームは `fragmentSize` を増やさないまま `fragments` に積まれる。1 バイトのメッセージに対して空フレームを送り続けると、上限 (`maxPayload`) に掛からずにバッファの配列が伸び続ける | 5 万フレームで 50001 個を保持 | 断片は 1 本の文字列へ足す。空フレームは何も増やさない |
| 2 | `ws/server.ts` | `Origin` を見ない。ブラウザは WebSocket に同一オリジン制限を掛けないので、悪意のあるページからローカルサービスへ接続できる (Cross-Site WebSocket Hijacking) | `Origin: https://evil.example` で 101 | `allowed_origins` を設定すると、載っていない Origin を 403 |
| 3 | `http/server.ts` | `Host` を見ない。`127.0.0.1` で待ち受けるサービスは DNS リバインディングでブラウザから呼べる | コード上、検査なし | `allowed_hosts` を設定すると、載っていない Host を 421 |
| 4 | `http/server.ts` `buildContext` | ボディを `ctx.query` へ後から足すため、ボディのキーがパス変数を上書きする。`/user/{id:number}` に `{"id":999}` を送ると `ctx.query.id` が 999 になる (`ctx.params.id` は 7 のまま) | `{"query":999,"params":7}` | パス変数と同じ名前のキーはボディから取り込まない |
| 5 | `ws/server.ts` `onUpgrade` | `attach` (先読み分の処理) の後で `connection` を発火する。ハンドシェイクと同じパケットで届いたフレームは、リスナーが付く前に処理されて失われる | 同時に送ったメッセージが届かない | 先に `on_connection` を呼び、その後で先読み分を渡す |
| 6 | `log/sink.ts` `DailyFileSink` | `createWriteStream` の `error` を受けていない。ログの出力先が書けなくなる (権限・ディスク・ディレクトリ削除) と未捕捉例外になり、`installProcessHandlers` がプロセスを終了させる | `uncaughtException` (ENOENT) | 開けない・書けないときは捨てる。サービスは止めない |
| 7 | `log/format.ts` `formatPretty` | 文字列の値とメッセージをそのまま出す。改行を含む値で、偽のログ行を作れる | 1 レコードが 2 行になる | 制御文字をエスケープし、1 レコード 1 行を保つ |
| 8 | `log/record.ts` | マスク対象が完全一致のみで、`cookie` `set-cookie` `x-api-key` `access_token` `refresh_token` が漏れる (ヘッダをそのままログに入れたとき) | そのまま出力される | 対象に追加 |
| 9 | `ws/socket.ts` | close フレームの検査が無い: 1 バイトの本文、送ってはいけないコード (1005 など)、理由の不正な UTF-8 を受け入れる。`close()` は理由を 125 バイトで機械的に切るので、UTF-8 の文字の途中で切れる | コード上 | 1002 / 1007 で切断。理由は文字の境界で切る |
| 10 | `ws/socket.ts` `write` | 送信待ちに上限が無い。読まない相手へ送り続けると Node のバッファが伸びる | コード上 | 送信待ちが `max_buffered` (既定 16MiB) を超えたら切断 |
| 11 | `ws/frame.ts` `redis/resp.ts` | 受信のたびに `Buffer.concat` で全体を作り直す。RESP は未完の応答を毎回先頭から解析し直す。小刻みに届く大きな応答で 2 乗時間になる | コード上 | フレームは読み位置を進める方式。RESP は入れ子の深さにも上限 (32) を置いた。再解析は残る |
| 12 | `http/body.ts` `mergeBody` | `URLSearchParams` は例外を投げないので `false` を返す経路に到達しない。JSON でないボディはすべてフォームとして解釈される。JSON の配列は `"0"` `"1"` というキーで足される | コード上 | オブジェクト以外の JSON は足さない (生のボディは `ctx.body`)。戻り値は無くした |
| 13 | `http/server.ts` | 要求を受け切るまでの時間・同時接続数は Node の既定値任せで、Portus としての上限が無い | コード上 | 要求 30 秒・応答後 5 秒・同時接続 1024・ワーカー待ち 1024 を既定に持つ |

重さの目安: 1・2・3 は外から突ける。4 はハンドラが `ctx.query` を信用していると認可の抜けになる。6 は運用中の停止要因。残りは堅牢性。

## 2. C++ 版で意図的に変えた点

- **chunked の要求は受けない (411)。** 前段のプロキシと長さの解釈が食い違うと、別の要求を紛れ込まされる。ブラウザと各クライアントは長さの分かる要求に `Content-Length` を付ける
- **ハンドラは同期関数。** `Promise` の代わりにワーカースレッドでブロックする。Redis / fetch / S3 / Push も完了まで戻らない呼び出しにした
- **ホットリロードは移していない。** ESM の読み直しに依存した開発専用の機能
- **WebSocket クライアントは `ws://` のみ。** 非ブロッキングの TLS を持たないため
- **`ctx.req` / `ctx.res` (Node のオブジェクト) は無い。** ヘッダは `ctx.headers`、応答は `Reply` で返す
- **ログの書き込みは専用スレッド。** 詰まったら捨てて件数を残す
- **JSON は自作。** 深さ 64 まで、同じキーは後勝ち、キー数が多くても 2 乗時間にならない

## 3. C++ 版そのものの検証

- TypeScript 版のテストを移した上で、上の表の各点と、C++ で新しく生まれる危険 (寿命・スレッド) のテストを足した (64 件)
- 仕様の検証値: RFC 6455 (Sec-WebSocket-Accept)、RFC 8291 Appendix A (Web Push 暗号文)、AWS ドキュメントの SigV4 署名例
- TLS: 自己署名 CA を使うテスト用サーバで、信頼していない発行元とホスト名の不一致を拒否することを確認
- gcc の AddressSanitizer + UndefinedBehaviorSanitizer、ThreadSanitizer で全テスト通過。clang でもビルド・テスト通過
- 未確認: Windows / macOS の実機、実際の外部サービス (S3 / push サービス / 各 AI API / Redis 本体) への通信
