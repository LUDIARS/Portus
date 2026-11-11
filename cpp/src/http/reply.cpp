#include "portus/http.hpp"

namespace portus {

static HeaderList with_type(const char* type, HeaderList headers) {
	HeaderList out;
	out.emplace_back("content-type", type);
	for (auto& kv : headers) out.push_back(std::move(kv));
	return out;
}

Reply::Reply(int status_, std::string body_, HeaderList headers_) : status(status_), body(std::move(body_)), headers(std::move(headers_)) {}

Reply::Reply(const char* text_body) : Reply(text(text_body)) {}

Reply::Reply(std::string text_body) : Reply(text(std::move(text_body))) {}

Reply::Reply(const Json& data) : Reply(json(data)) {}

Reply Reply::bytes(std::string data) {
	return Reply(200, std::move(data), with_type("application/octet-stream", {}));
}

Reply json(const Json& data, int status, HeaderList headers) {
	return Reply(status, data.dump(), with_type("application/json; charset=utf-8", std::move(headers)));
}

Reply text(std::string body, int status, HeaderList headers) {
	return Reply(status, std::move(body), with_type("text/plain; charset=utf-8", std::move(headers)));
}

Reply html(std::string body, int status, HeaderList headers) {
	return Reply(status, std::move(body), with_type("text/html; charset=utf-8", std::move(headers)));
}

}
