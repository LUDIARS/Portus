# キーセットのページ読み

対象タスク `actio:4c7f009e-2bfc-4e45-bc6f-d84681ce120a`。Actio の `GET /api/tasks` は、認可済みの結果から一行先読みして次のページの有無を判定する。

`<portus/keyset.hpp>` の `read_keyset_page<Row>` に、検証済みの件数、アプリの上限、DB を読む関数を渡す。関数は同期的に一度呼ばれ、所有権のある `std::vector<Row>` を返す。Actio は DB ワーカー上でこの関数を呼び、接続・トランザクションはそのワーカーが所有する。

```cpp
auto page = portus::read_keyset_page<TaskSummary>(limit, 500, [&](std::size_t count) {
    return repository.list_after(viewer, filters, sort, cursor, count);
});
if (page.has_more) next_cursor = encode_task_cursor(sort, page.rows.back());
```

Actio は limit 未指定時に 100 を使い、文字列表現を検証してから整数を渡す。0、上限超過、先読み件数のオーバーフローで Portus は DB に触れる前に `std::invalid_argument` を送出する。DB は権限・検索条件・カーソル条件・一意なタイブレークを含む順序を適用してから、渡された件数を SQL の LIMIT に束縛する。過剰な行を返すアダプターは `std::length_error` になり、DB の例外は呼び出し元へ伝播する。最終ページでは Actio が nextCursor を null にする。

libpq、期限なしの並べ方、カーソルの JSON 形式、認証・セッション、キャッシュのキーと無効化は Actio が所有する。既存の HttpServer、Reply、Redis、環境設定 API を再利用する。同一ポートの /ws は接続引き渡しと認証コンテキストを扱う独立した変更として扱う。
