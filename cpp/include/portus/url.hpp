#pragma once
#include <string>

namespace portus {

struct Url {
	std::string scheme;		//小文字
	std::string userinfo;	//"user:pass" (無ければ空)
	std::string host;		//小文字。IPv6 は [] を外したもの
	int port = 0;			//省略時はスキームの既定 (http/ws = 80, https/wss = 443, mqtt = 1883)
	std::string target;		//パス + クエリ (最低でも "/")

	std::string host_header() const;	//Host ヘッダの値 (既定ポートは省く)
	std::string origin() const;			//scheme://host[:port]
};

//絶対 URL を分解する。解釈できなければ false
bool parse_url(const std::string& text, Url& out);

}
