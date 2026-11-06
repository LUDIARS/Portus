# 依存パッケージ脆弱性棚卸し (2026-10-31)

参照元: [vtn-team/VTN-Connect](https://github.com/vtn-team/VTN-Connect) の `NodeJSServer` (Node.js 製のゲーム/API サーバ)。
その npm 依存を、過去の脆弱性報告 (CVE / GHSA) と攻撃面の大きさで評価し、
使っている機能だけを Portus に自作して置き換えられるようにした記録。参照元リポ自体は変更していない。

## 方針

- **使っている機能だけを作る。** 汎用ライブラリの未使用機能 (拡張・プロキシ・リトライ・型生成など) は持たない。
- **暗号・圧縮は自作のプリミティブを書かない。** AES-GCM / ECDH / HKDF / ECDSA / SHA / HMAC は Node 本体の `node:crypto` (OpenSSL) を使い、
  その上の「プロトコル組み立て」(Web Push の暗号化手順、VAPID の JWT、SigV4 署名) だけを自作する。
  暗号プリミティブの自作は脆弱性を減らすどころか増やすため採らない。圧縮が必要になったら `node:zlib` を使う (現状は不要)。
- **ルーティングは表に書いたものだけが到達可能。** 参照元のツリー型ルータにあった「見つからなければ親の `/` に落ちる」挙動は持たない。
- バージョンは `package.json` で完全固定 (`^` なし)。

## 依存の評価と処置

「過去の報告」は、そのパッケージ本体または推移的依存で報告された代表的なもの。
ロック済みバージョン (旧 `yarn.lock`) で影響を受けるものには ⚠ を付けた。

| パッケージ (旧ロック版) | 過去の脆弱性報告 | 使っていた機能 | 処置 |
|---|---|---|---|
| `ws` 8.18.0 | CVE-2024-37890 (大量ヘッダで DoS)、CVE-2021-32640 (Sec-WebSocket-Protocol の ReDoS)、CVE-2016-10542 (巨大ペイロードで DoS)、CVE-2016-10518 (ping で未初期化メモリ露出) | サーバ・クライアント、text 送受信、ping/pong | **自作** `portus/ws` (RFC 6455)。拡張なし、最大ペイロード 1MiB (ws の既定は 100MiB) |
| `web-push` 3.6.7 | 推移的依存: `minimist` CVE-2021-44906 (プロトタイプ汚染)、`https-proxy-agent`・`asn1.js`・`jws`・`http_ece` を同梱 | VAPID 署名・aes128gcm 暗号化・送信 | **自作** `portus/push` (RFC 8291 / 8188 / 8292)。暗号は `node:crypto` |
| `aws-sdk` v2 2.1692.0 | 2025-09 にサポート終了 (EOL)。CVE-2020-28472 (設定のプロトタイプ汚染)、推移的依存 `xml2js` CVE-2023-0842 (プロトタイプ汚染) | S3 PutObject 1 か所 | **自作** `portus/aws` (SigV4 + PutObject、認証情報は 環境変数 → `~/.aws/credentials` → IMDSv2) |
| `openai` 4.x / `@anthropic-ai/sdk` 0.36 | 推移的依存 `node-fetch` v2 CVE-2022-0235 (リダイレクト先へ Authorization 漏えい)・CVE-2020-15168、⚠ `form-data` 4.0.2 CVE-2025-7783 (予測可能な boundary) | models 一覧、chat/messages、realtime session | **自作** `portus/ai` (`OpenAIClient` `AnthropicClient`) (標準 fetch、リダイレクト禁止・タイムアウト付き) |
| `@google/generative-ai` 0.21 | 非推奨 (後継 `@google/genai`)。API キーを URL クエリに載せる箇所があった | generateContent、models 一覧 | **自作** `portus/ai` (`GeminiClient`)。キーはヘッダ渡し、モデル名を検証してからパスに埋め込む |
| `@notionhq/client` 2.x | 推移的依存 `node-fetch` v2 (上記) | users / databases.query / blocks.children / search / pages | **自作** `portus/ai` (`NotionClient`、SDK と同名メソッドのドロップイン) |
| `redis` 4.7.0 | CVE-2021-29469 (monitor モードの ReDoS、v3 系) | GET / SET / HSET / HGETALL | **自作** `portus/redis` (RESP2、長さ上限付き) |
| `node-fetch` 2.7.0 | CVE-2022-0235、CVE-2020-15168 | 未使用 (Node 22 の fetch を使用) | **削除** |
| `uuid` 8.3.2 / `uuidv4` | `uuidv4` は保守終了 | v4 生成のみ | **削除** → `crypto.randomUUID()` (`portus/id`) |
| `dotenv` 16 | 報告なし (攻撃面縮小のため) | `.env` 読み込み | **自作** `portus/env` (KEY=VALUE のみ) |
| `sqlite` / `sqlite3` 5.1.7 | CVE-2022-43441 (バインド時のコード実行)、ビルド時の `tar` CVE-2021-32803/32804/37701/37712/37713 | 未使用 (コメントアウト済み) | **削除** |
| `cron` 1.8.3 | 推移的依存 `moment` CVE-2022-24785 (ロケールのパストラバーサル)・CVE-2022-31129 (ReDoS) | 未使用 | **削除** |
| `jest` 28 / `ts-jest` | 推移的依存 `braces` CVE-2024-4068、`micromatch` CVE-2024-4067、`cross-spawn` CVE-2024-21538、`json5` CVE-2022-46175、`@babel/traverse` CVE-2023-45133。しかも `dependencies` に入っていて本番にも入っていた | テスト | **不採用** → `node:test` |
| `websocket` (dev) | 推移的依存 `es5-ext` (postinstall の protestware) | テストクライアント | **不採用** → `portus/ws` のクライアント |
| `ts-node-dev` (dev) | 推移的依存 (`chokidar` 系) | 未使用 | **削除** |
| `mariadb` | 報告なし | DB 接続 | **自作しない** (利用側で 3.5.4 などに固定して使う)。プロトコル実装と認証まわりは自作の利得よりリスクが大きい |
| `typescript` / `@types/node` | — | ビルドのみ | Portus の開発依存 (固定)。実行時依存はゼロ |

参照元に Portus を適用すると、直接依存は 18 → 1 (`mariadb`) になる (VTN-Connect のローカル検証ブランチで確認。参照元へは反映していない)。

## 参照元で見つかった穴と Portus の既定動作

| 参照元の箇所 | 問題 | Portus の既定動作 |
|---|---|---|
| HTTP ボディ | サイズ無制限 (メモリ枯渇 DoS) | `maxBodyBytes` 既定 1MiB、超過は 413 |
| ボディ・クエリの取り込み | `route.query[k] = d[k]` に `__proto__` を入れられた (プロトタイプ汚染) | null プロトタイプのオブジェクト + 危険キーの除外 (`assignSafe`) |
| ルーティング | 未知のパスが親ルートの `/` に落ち、型が合わない変数は先頭のルートへ流れた | 表に完全一致したものだけ。`{id:number}` に数値以外は 404 |
| アクセスログ | クエリごと出力し、`session`・`token` がログに残った | パスのみ記録。構造化フィールドの秘匿キーは `[REDACTED]` |
| Web Push 送信 | クライアントが登録した任意の URL へサーバが POST する (SSRF) | https + 既知の push サービスのみ (`allowedHosts` で変更可) |
| ハンドラ例外 | 未処理の reject でレスポンスが返らない / プロセスが落ちる | 500 を返し、リクエスト ID 付きでエラーログ |
| Gemini | API キーを URL クエリに載せていた | `x-goog-api-key` ヘッダで渡す。モデル名は検証してからパスへ |
| WebSocket | `ws` の最大ペイロード既定 100MiB | 既定 1MiB、超過は 1009 で切断 |
