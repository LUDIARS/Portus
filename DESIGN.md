# Portus 設計

## 0. 目的

Node サーバが抱える脆弱性の大半は、自分のコードではなく **巨大な依存ツリー** から来る。
Portus は、サーバで実際に使う機能だけを小さく自作して依存ツリーを切り、
「何が到達可能で、何がログに出て、どこへ通信するか」をコードを読めば分かる状態にする。

参照元は vtn-team/VTN-Connect の NodeJSServer。棚卸しの結果は `docs/dependency-audit.md`。

## 1. 決め事

### 1.1 実行時依存はゼロ

- `dependencies` を持たない。開発依存は `typescript` と `@types/node` だけ (バージョン完全固定)
- 新しい依存を足すときは、`docs/dependency-audit.md` に過去の脆弱性と代替案を書いてから

### 1.2 暗号・圧縮のプリミティブは自作しない

- AES-GCM / ECDH / HKDF / ECDSA / SHA-2 / HMAC / 乱数は `node:crypto` (OpenSSL) を使う
- 自作するのは、その上の **プロトコル組み立て** (RFC 8291 の鍵導出手順、VAPID の JWT、AWS SigV4) だけ
- 圧縮が必要になったら `node:zlib` を使う
- 暗号プリミティブを自作すると、タイミング差やパディングの扱いで新しい脆弱性を作るため採らない

### 1.3 使う機能だけを作る

- 汎用ライブラリの網羅性 (拡張・プロキシ・リトライ・全 API の型) は持たない
- 「あると便利」ではなく「呼ばれている」ものだけを足す。足すときはテストを一緒に

### 1.4 透明なルーティング

- ルートは 1 行 1 ルートの表。上から評価して最初の一致を使う
- 親ルートへのフォールバック・型不一致時の別ルート流入はしない
- `"モジュール#関数"` は `[A-Za-z0-9_]` だけを許し、表に載ったモジュールしか読み込まない (任意ファイルの読み込みを防ぐ)
- `router.list()` で表をそのまま公開できる

### 1.5 安全側の既定値

| 項目 | 既定 |
|---|---|
| HTTP 待ち受け | `127.0.0.1` (前段のリバースプロキシ経由を想定) |
| HTTP ボディ | 1MiB、超過 413 |
| WebSocket ペイロード | 1MiB、超過は 1009 で切断。マスク無し・RSV・不正 UTF-8 は切断 |
| 外部 fetch | リダイレクト禁止、タイムアウト付き、エラー本文は 500 文字で切る |
| Web Push 送信先 | https の既知 push サービスのみ |
| 外部入力のオブジェクト化 | null プロトタイプ + `__proto__` / `constructor` / `prototype` を捨てる |
| ログ | 秘匿キーをマスク。HTTP アクセスログにボディ・クエリを出さない |

### 1.6 ホットリロードは開発専用

- ESM はモジュールキャッシュを消せないため、`import("file?v=N")` で別モジュールとして読み直す。古い版はメモリに残る
- 差し替えるのは状態を持たない層 (ハンドラ・ルート表) だけ。状態を持つ層は `portus-dev` が再起動する
- 壊れたルート表は採用せず、前の表で動き続ける

## 2. 構成

```
src/
  http/   types (契約) / router / body / reply / middleware / handlerResolver / server
  ws/     frame / handshake / socket / server / client
  push/   encrypt (RFC 8291) / vapid (RFC 8292) / endpoint (送信先の許可) / send
  aws/    sigv4 / credentials / s3
  redis/  resp / client
  ai/     openai / anthropic / gemini / notion
  net/    fetchJson
  log/    level / record (マスク) / format / sink / logger / consoleBridge / process
  hot/    versionedImport / handlerRegistry / routeReloader / watcher
  env.ts / id.ts / safeObject.ts
bin/portus-dev.js   tsc --watch + 再起動 / ホットスワップの振り分け
```

各ファイルは 1 つの責任だけを持つ (LUDIARS コード規約の SRP)。

## 3. 検証

- 仕様の検証値と突き合わせる: RFC 6455 (Sec-WebSocket-Accept)、RFC 8291 Appendix A (Web Push 暗号文)、AWS ドキュメントの SigV4 署名例
- HTTP / WebSocket は実ソケットで往復させる (ループバック、空きポート)
- 外部サービス (S3 / Redis / 各 AI API / push サービス) への実通信はテストに含めない
