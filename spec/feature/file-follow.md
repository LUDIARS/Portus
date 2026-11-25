# ファイル追従

Actio `actio:728075d7-f006-4dc9-8438-c0e8fb1e1ebe`。Vestigium の VictoriaLogs 送信で使う C++ API。

`portus/file_follow.hpp` の `portus::FileFollower` は通常ファイルを開き、`snapshot()` で識別子と開いた時点の長さを返す。Windows は同じハンドルからボリュームシリアル番号と `FileIdInfo` の 128 ビット ID を取得する。読み取り・書き込み・削除を共有する。POSIX は同じディスクリプタから `st_dev` と `st_ino` を取得する。改名とハードリンクで識別子は変わらない。

`read(offset, byte_limit)` は、指定位置から開いた時点の長さまでを正のバイト上限内で読み、最後の LF までのバイト列と `next_offset` を返す。CRLF を含め、改行を構成するバイトも上限と位置に含む。書き込み途中の末尾は次の走査に回す。次の走査ではパスから開き直し、識別子と永続化した位置を照合する。

- `complete` は読み出せた確定行を返す。上限内に確定行がない場合は空になる。
- `line_too_long` は、先頭行に上限内の LF がなく、走査対象の残りが上限を超える場合に返す。前に確定行があれば先に返し、次の読み取りで該当行を診断する。
- `truncated` は、保存した位置よりファイルが短くなった場合に返す。読んだ位置と同じ長さは有効なファイル末尾として扱う。

空の結果では位置を維持する。確定行を受け取った呼び出し側は、処理の成功後に `next_offset` を保存する。走査中に切り詰めと再拡張が続き、長さの取得時点ですでに元の長さ以上に戻っている場合は、長さの比較だけでは検出できない。I/O エラーは `std::system_error`、上限ゼロは `std::invalid_argument` で伝える。

## 受け入れ条件

- C-3 follow_window(snapshot_size, current_size, offset, byte_limit): 切り詰めを検出し、読み取り範囲を開いた時点の長さ・現在の長さ・バイト上限の内側に収める
- C-4 follow_prefix(bytes, byte_limit, has_more): 改行済みの行だけを返し、上限を超える先頭行は位置を進めず通知する
