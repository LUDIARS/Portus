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
| 外部への要求 | 自前の HTTP/1.1 (`fetch` は使わない)。TLS 1.3 以上、圧縮・リダイレクトなし、内部向けアドレスと平文は明示したときだけ。本文 8 MiB・ヘッダ 16 KiB、接続 10 秒・全体 120 秒の期限、接続先ごとに同時 16 本。エラー本文は 500 文字まで読んで閉じる |
| Web Push 送信先 | https の既知 push サービスのみ |
| 外部入力のオブジェクト化 | null プロトタイプ + `__proto__` / `constructor` / `prototype` を捨てる |
| ログ | 秘匿キーをマスク。HTTP アクセスログにボディ・クエリを出さない |

### 1.6 外部への要求は自前の HTTP/1.1

- Node 組み込みの `fetch` (undici) は使わない。応答の圧縮を勝手に展開し (Node 同梱の zlib / Brotli を通る)、本文の大きさに上限が無いため
- `node:tls` / `node:net` の上で HTTP/1.1 を組み立てる。TLS (暗号) は `node:tls` (OpenSSL) に任せ、自作するのは要求の組み立てと応答の解析だけ (§1.2)
- 考え方は QUIC に近い: 通信の仕組みを自分たちのコードで持つ / 暗号化が前提 (TLS 1.3 以上) / ストリームごとに受け取る量を明示する (本文は読んだ分だけ・上限つき) / 同時ストリーム数の上限 / 1 本の遅れが他を止めない (要求ごとに独立した接続) / 張り直しはセッション再開で軽くする (0-RTT は使わない) / 曖昧な形を受けない
- 層は 3 つ: 接続 (`connection`: 名前解決で固定した IP へ TCP・TLS) → HTTP/1.1 の枠組み (`http1`) → 中身 (`fetchJson`・`sse`)
- 応答の解析は厳格にする (長さの分からない本文・圧縮・`Transfer-Encoding` と `Content-Length` の両方・折り返しヘッダは拒否)。テストは全バイト位置での分割と上限ちょうど / +1 を必ず置く
- プロキシ・HTTP/2・接続の使い回しは持たない (使っていないため)

### 1.7 ホットリロードは開発専用

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
  net/    errors / address (名前解決と内部アドレスの拒否) / limiter (同時実行数) / tlsSessions (セッション再開) /
          connection (TCP・TLS 接続) / http1 (要求の組み立て・応答の解析) / request / sse / fetchJson
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
