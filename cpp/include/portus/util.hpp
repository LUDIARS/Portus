#pragma once
//文字列まわりの小さな共通処理
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace portus {

std::string to_lower(std::string_view s);
std::string_view trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);

//厳密な UTF-8 か (冗長表現・サロゲート・U+10FFFF 超を拒否)
bool utf8_valid(std::string_view s);

//decodeURIComponent 相当。不正な % 列・不正な UTF-8 は false
bool percent_decode_strict(std::string_view s, std::string& out);
//クエリ文字列用。+ を空白にし、不正な % 列はそのまま残す
std::string percent_decode_form(std::string_view s);
//encodeURIComponent 相当 (A-Z a-z 0-9 - _ . ! ~ * ' ( ) 以外をエンコード)
std::string percent_encode_component(std::string_view s);

//外部入力のキーとして受け取らない名前 (JS 側へ渡ったときのプロトタイプ汚染を防ぐ)
bool is_safe_key(std::string_view key);

std::int64_t now_ms();	//UNIX エポックからのミリ秒
std::string iso_time(std::int64_t epoch_ms);	//2026-01-02T03:04:05.678Z

}
