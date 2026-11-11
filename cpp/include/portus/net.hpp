#pragma once
//外部 API 呼び出し用の HTTP(S) クライアント。タイムアウト・リダイレクト禁止・応答サイズ上限・証明書検証を揃える
//呼び出しは完了まで戻らない (ハンドラ = ワーカースレッドから呼ぶ。イベントループからは呼ばない)
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

#include "portus/http_message.hpp"
#include "portus/json.hpp"

namespace portus {

struct FetchRequest {
	std::string method = "GET";
	HeaderList headers;
	std::string body;
	int timeout_ms = 120000;	//接続から応答を読み切るまでの合計
	std::size_t max_response_bytes = 16 * 1024 * 1024;
	std::string ca_file;		//https の検証に使う CA (PEM)。空なら OS / OpenSSL の既定
};

struct FetchResponse {
	int status = 0;
	Headers headers;
	std::string body;
	bool ok() const { return status >= 200 && status < 300; }
};

//通信の失敗・時間切れ・証明書の検証失敗・リダイレクト応答
class FetchError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//http:// / https:// へ 1 往復する。リダイレクトには従わない (3xx は FetchError)
FetchResponse fetch(const std::string& url, const FetchRequest& request = {});

//2xx 以外の応答
class HttpError : public std::runtime_error {
public:
	HttpError(const std::string& message, int status_, std::string body_) : std::runtime_error(message), status(status_), body(std::move(body_)) {}
	int status;
	std::string body;	//先頭 500 バイトまで
};

struct JsonRequest {
	std::string method;			//空なら body の有無で POST / GET
	HeaderList headers;
	std::optional<Json> body;
	int timeout_ms = 120000;
	std::string ca_file;
};

//JSON を送って JSON を受ける。2xx 以外は HttpError、空の応答は null
Json fetch_json(const std::string& url, const JsonRequest& request = {});

//URL パスに埋め込む識別子 (モデル名など) を検証する。[A-Za-z0-9._:-]{1,128} 以外は std::invalid_argument
std::string path_segment(const std::string& value, const std::string& name);

}
