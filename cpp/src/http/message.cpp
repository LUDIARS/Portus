#include "portus/http_message.hpp"

#include "portus/util.hpp"

namespace portus {

void Headers::add(std::string_view name, std::string_view value) {
	items_.emplace_back(to_lower(name), std::string(value));
}

const std::string* Headers::find(std::string_view lower_name) const {
	for (const auto& kv : items_) {
		if (kv.first == lower_name) return &kv.second;
	}
	return nullptr;
}

std::string Headers::get(std::string_view lower_name, const std::string& fallback) const {
	const std::string* v = find(lower_name);
	return v ? *v : fallback;
}

std::size_t Headers::count(std::string_view lower_name) const {
	std::size_t n = 0;
	for (const auto& kv : items_) {
		if (kv.first == lower_name) ++n;
	}
	return n;
}

bool is_valid_header_name(std::string_view name) {
	if (name.empty()) return false;
	for (const char c : name) {
		const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!alnum && std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos) return false;
	}
	return true;
}

bool is_valid_header_value(std::string_view value) {
	for (const char ch : value) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if ((c < 0x20 && c != '\t') || c == 0x7F) return false;
	}
	return true;
}

static const std::size_t kMaxHeaders = 100;

//空行までを取り出して行に分ける。first に先頭行、headers に残りを入れる
static ParseResult split_head(std::string_view buf, std::size_t max_bytes, std::string_view& first, Headers& headers, std::size_t& consumed) {
	const std::size_t end = buf.find("\r\n\r\n");
	if (end == std::string_view::npos) return buf.size() > max_bytes ? ParseResult::Bad : ParseResult::Incomplete;
	if (end + 4 > max_bytes) return ParseResult::Bad;
	consumed = end + 4;

	const std::string_view head = buf.substr(0, end);
	std::size_t pos = head.find("\r\n");
	first = head.substr(0, pos);

	std::size_t lines = 0;
	while (pos != std::string_view::npos) {
		const std::size_t start = pos + 2;
		pos = head.find("\r\n", start);
		const std::string_view line = head.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);

		if (++lines > kMaxHeaders) return ParseResult::Bad;
		const std::size_t colon = line.find(':');
		if (colon == std::string_view::npos) return ParseResult::Bad;
		const std::string_view name = line.substr(0, colon);
		const std::string_view value = trim(line.substr(colon + 1));
		//名前の前後の空白・行の折り返しは、解釈の食い違い (リクエストスマグリング) の元なので受けない
		if (!is_valid_header_name(name) || !is_valid_header_value(value)) return ParseResult::Bad;
		headers.add(name, value);
	}
	return ParseResult::Ok;
}

ParseResult parse_request_head(std::string_view buf, std::size_t max_bytes, RequestHead& out, std::size_t& consumed) {
	std::string_view first;
	Headers headers;
	const ParseResult r = split_head(buf, max_bytes, first, headers, consumed);
	if (r != ParseResult::Ok) return r;

	const std::size_t sp1 = first.find(' ');
	const std::size_t sp2 = first.rfind(' ');
	if (sp1 == std::string_view::npos || sp2 == sp1) return ParseResult::Bad;
	const std::string_view method = first.substr(0, sp1);
	const std::string_view target = first.substr(sp1 + 1, sp2 - sp1 - 1);
	const std::string_view version = first.substr(sp2 + 1);

	if (method.empty() || method.size() > 16) return ParseResult::Bad;
	for (const char c : method) {
		if (c < 'A' || c > 'Z') return ParseResult::Bad;
	}
	if (target.empty() || target[0] != '/') return ParseResult::Bad;
	for (const char ch : target) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c <= 0x20 || c == 0x7F) return ParseResult::Bad;
	}
	if (version != "HTTP/1.1" && version != "HTTP/1.0") return ParseResult::Bad;

	out.method = std::string(method);
	out.target = std::string(target);
	out.minor_version = version == "HTTP/1.1" ? 1 : 0;
	out.headers = std::move(headers);
	return ParseResult::Ok;
}

ParseResult parse_response_head(std::string_view buf, std::size_t max_bytes, ResponseHead& out, std::size_t& consumed) {
	std::string_view first;
	Headers headers;
	const ParseResult r = split_head(buf, max_bytes, first, headers, consumed);
	if (r != ParseResult::Ok) return r;

	//"HTTP/1.1 200 OK"
	if (first.size() < 12 || !starts_with(first, "HTTP/1.") || first[8] != ' ') return ParseResult::Bad;
	int status = 0;
	for (std::size_t i = 9; i < 12; ++i) {
		if (first[i] < '0' || first[i] > '9') return ParseResult::Bad;
		status = status * 10 + (first[i] - '0');
	}
	if (first.size() > 12 && first[12] != ' ') return ParseResult::Bad;

	out.status = status;
	out.headers = std::move(headers);
	return ParseResult::Ok;
}

bool parse_content_length(const Headers& headers, std::uint64_t& out) {
	out = 0;
	bool seen = false;
	for (const auto& kv : headers.all()) {
		if (kv.first != "content-length") continue;
		const std::string& v = kv.second;
		if (v.empty() || v.size() > 18) return false;
		std::uint64_t n = 0;
		for (const char c : v) {
			if (c < '0' || c > '9') return false;
			n = n * 10 + static_cast<std::uint64_t>(c - '0');
		}
		if (seen && n != out) return false;
		seen = true;
		out = n;
	}
	return true;
}

bool header_has_token(std::string_view value, std::string_view token) {
	for (const std::string& part : split(value, ',')) {
		if (to_lower(trim(part)) == token) return true;
	}
	return false;
}

const char* status_text(int status) {
	switch (status) {
	case 100: return "Continue";
	case 101: return "Switching Protocols";
	case 200: return "OK";
	case 201: return "Created";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 307: return "Temporary Redirect";
	case 308: return "Permanent Redirect";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 415: return "Unsupported Media Type";
	case 418: return "I'm a teapot";
	case 421: return "Misdirected Request";
	case 422: return "Unprocessable Entity";
	case 426: return "Upgrade Required";
	case 429: return "Too Many Requests";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	}
	return "Status";
}

}
