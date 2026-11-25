# Actio の PostgreSQL アダプター

対象タスク `actio:3774d57e-b586-4c18-96ed-74b00ac5eee7`。公開ヘッダーは `portus/db.hpp`。Actio が libpq を使って `DbPool` と `DbConnection` を実装する。接続数は最大 10。プールは複数ワーカーから安全に取得できるようにし、待機・接続を含めて期限内に排他的な接続を返す。取得失敗は例外で返す。

```cpp
// ワーカー内。team_id と after_id はこの呼び出し中有効な文字列。
using namespace portus;
using namespace std::chrono;
auto connection = pool.acquire(steady_clock::now() + seconds(10));
auto page = read_keyset_page<DbRow>(limit, 500, [&](std::size_t count) {
    auto n = std::to_string(count);
    return connection->query({
        "SELECT id FROM tasks WHERE team_id = $1 AND id > $2 ORDER BY id LIMIT $3",
        {team_id, after_id, n}, count
    }, steady_clock::now() + seconds(7));
});
```

この例は ID 順の次ページ取得を示す。実際の一覧では Actio の認可条件とソート・カーソルの組を SQL に組み込む。列は SELECT 順に `row.at(index)` で読む。SQL NULL は `std::nullopt`、空文字は長さゼロの値になる。行が返らない更新の `max_rows` は 0。

**読み取り境界**　アダプターは SQL と UTF-8 テキストの値を分けてバインドする。借用した文字列を同期呼び出し中だけ使い、終端文字が必要な libpq API には長さを確認して所有文字列を渡す。行を逐次取得し、列数・値の長さを確認してから一行のビューを `DbResult::append` に渡す。Portus はコピー前に行・列・値・累積バイト数を検査する。上限値はヘッダーに定義している。入力を組み立てる側も、パラメーター配列や文字列の確保前に同じ上限を適用する。

**更新と障害**　ワーカーが取得した同じ接続で `begin(deadline)`、更新と監査記録の `query`、`commit(deadline)` を順に呼ぶ。途中失敗は `rollback(deadline)`。アダプターは未完了のトランザクションを持つ接続を返却時に破棄またはロールバックし、復旧を確認した接続だけをプールに戻す。各操作は単調時計の絶対期限を使う。ドライバーは待機中も期限を適用し、期限切れは `DbTimeout`、切断は `DbDisconnected` で通知する。コミット応答を失った更新は対象 ID を照会して結果を確定する。例外文に SQL パラメーターや接続文字列を含めない。

**スレッド**　有界の `ThreadPool::submit` で DB 作業を渡し、キュー満杯なら受付を打ち切る。接続を取得したワーカーで全 DB 操作と返却を終える。所有権のある行または失敗を `EventLoop::post` で戻し、HTTP 接続の生存確認と応答はイベントループ上で行う。
