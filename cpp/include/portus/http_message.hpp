#pragma once
//HTTP/1.1 のヘッダ部の解析 (サーバの要求行・クライアントの状態行)。上限と文字種を厳しく見る
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace portus {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

//ヘッダの並び。名前は小文字で持つ
class Headers {
public:
	void add(std::string_view name, std::string_view value);
	const std::string* find(std::string_view lower_name) const;	//最初の 1 つ。無ければ nullptr
	std::string get(std::string_view lower_name, const std::string& fallback = "") const;
	std::size_t count(std::string_view lower_name) const;
	const std::vector<std::pair<std::string, std::string>>& all() const { return items_; }

private:
	std::vector<std::pair<std::string, std::string>> items_;
};

struct RequestHead {
	std::string method;
	std::string target;		//パス + クエリ
	int minor_version = 1;	//HTTP/1.x の x
	Headers headers;
};

struct ResponseHead {
	int status = 0;
	Headers headers;
};

enum class ParseResult { Incomplete, Ok, Bad };

//buf の先頭からヘッダ部 (空行まで) を読む。Ok のとき consumed に読んだバイト数が入る
ParseResult parse_request_head(std::string_view buf, std::size_t max_bytes, RequestHead& out, std::size_t& consumed);
ParseResult parse_response_head(std::string_view buf, std::size_t max_bytes, ResponseHead& out, std::size_t& consumed);

//Content-Length を読む。無ければ 0 で true。数字以外・食い違う重複は false
bool parse_content_length(const Headers& headers, std::uint64_t& out);

//"a, B ,c" 形式のヘッダ値に token (小文字) が含まれるか
bool header_has_token(std::string_view value, std::string_view token);

//ヘッダの値として送ってよいか (改行などを含むと応答を分割されるため拒否する)
bool is_valid_header_value(std::string_view value);
bool is_valid_header_name(std::string_view name);

const char* status_text(int status);

}
