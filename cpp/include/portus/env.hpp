#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace portus {

//.env テキストを解析する。KEY=VALUE 形式だけ (変数展開・複数行値は扱わない)。同じキーは後勝ち
std::vector<std::pair<std::string, std::string>> parse_env(std::string_view text);

//.env を読んで環境変数に入れる (既に設定済みのキーは上書きしない)。ファイルが無ければ何もしない
void load_env(const std::string& file = ".env");

//環境変数を読む。無ければ fallback
std::string get_env(const char* name, const std::string& fallback = "");

}
