# Portus — Claude 向けメモ

LUDIARS の Node サーバ基盤ライブラリ。脆弱性報告の多い npm 依存を、使う機能だけの自作実装で置き換える。

## 鉄則 (踏み外し厳禁)

- **実行時依存を足さない。** `dependencies` は空のまま。開発依存も `typescript` / `@types/node` だけ。
- **暗号・圧縮のプリミティブを自作しない。** `node:crypto` / `node:zlib` を使い、自作はプロトコルの組み立てまで (DESIGN.md §1.2)。
- **使われていない機能を足さない。** 足すときは呼び出し元とテストを一緒に。
- **ルーティングにフォールバックを入れない。** 表に無いものは 404。
- **外部入力を通常のオブジェクトへ代入しない。** `assignSafe` / null プロトタイプを使う。
- **ログに秘匿値を出さない。** アクセスログにボディ・クエリを足さない。
- 参照元の vtn-team/VTN-Connect は参考にするだけで、変更しない。

## C++ 版 (`cpp/`)

鉄則は同じ。読み替えと追加だけ:

- 外部依存は OpenSSL だけ。ライブラリを同梱・取得しない (`FetchContent` / サブモジュール / vcpkg マニフェストを足さない)。
- 暗号は `src/core/crypto.cpp` から OpenSSL を呼ぶ。自作しない。
- 接続の状態はイベントループのスレッドだけが触る。他のスレッドからは `EventLoop::post` / `dispatch` で渡す。
- 外から来る入力で伸びるものには上限を置く (DESIGN.md §4.4)。
- コールバックが自分の持ち主を掴む形 (`shared_ptr` の循環) を作るときは、閉じたときに切れることを確認する。
- 変更したら `ctest` に加えて、ASan + UBSan と TSan でもテストを通す (下記)。

## テスト

- `npm test` (node:test)。外部サービスに繋がない。
- 暗号・署名は RFC / AWS の検証値と突き合わせるテストを必ず置く。
- C++ 版: `cmake -S cpp -B cpp/build && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure`。
  サニタイザは `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` と `-DCMAKE_CXX_FLAGS="-fsanitize=thread"` を別のビルドディレクトリで。

## branch 運用

- substantive な編集は feat/ ブランチ + PR (main 直編集しない)。
