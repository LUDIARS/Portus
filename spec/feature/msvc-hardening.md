# C++ の MSVC 堅牢化ビルド

関連タスクは `actio:1e5ff796-94c9-4efe-964a-f1071661791e`。

Portus の C++ ライブラリは、MSVC で Lorica の警告のエラー化を有効にした構成でビルドできること。Vestigium から `add_subdirectory` で取り込む構成も対象に含める。
公開ヘッダーの文字コードに合わせ、MSVC では `portus::portus` をリンクする C++ ターゲットにも `/utf-8` を適用する。

- `get_env` は未設定時に既定値を返す。`load_env` は既存の環境変数を保持する。Windows では CRT から取得したバッファを読み取り時に解放する。
- AWS のインスタンスメタデータから受け取る有効期限は UTC の秒までを読み、エポックミリ秒へ変換する。日時を読み取れない場合は既存の戻り値 `0` を返す。
- 日別ログのディレクトリとファイル名の接頭辞を UTF-8 として扱い、OS のパス表現へ変換する。

Vestigium の `vestigium-sink` の Release ビルドと、Portus の回帰テストで確認する。
