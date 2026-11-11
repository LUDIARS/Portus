#include "portus/net.hpp"

#include <chrono>

#include "channel.hpp"
#include "portus/url.hpp"
#include "portus/util.hpp"

namespace portus {

namespace {

const std::size_t kMaxHeadBytes = 64 * 1024;

//接続から読み溜めながら、行・固定長で取り出す
struct Reader {
	Channel& channel;
	std::string buf;
	std::size_t pos = 0;
	bool eof = false;

	bool fill() {
		if (eof) return false;
		char tmp[16384];
		const std::size_t n = channel.read(tmp, sizeof(tmp));
		if (n == 0) {
			eof = true;
			return false;
		}
		if (pos > 0) {
			buf.erase(0, pos);
			pos = 0;
		}
		buf.append(tmp, n);
		return true;
	}

	std::string read_line() {
		for (;;) {
			const std::size_t end = buf.find("\r\n", pos);
			if (end != std::string::npos) {
				std::string line = buf.substr(pos, end - pos);
				pos = end + 2;
				return line;
			}
			if (buf.size() - pos > kMaxHeadBytes || !fill()) throw FetchError("invalid response");
		}
	}

	void read_exact(std::size_t n, std::string& out) {
		while (buf.size() - pos < n) {
			if (!fill()) throw FetchError("response truncated");
		}
		out.append(buf, pos, n);
		pos += n;
	}
};

ResponseHead read_head(Reader& r) {
	for (;;) {
		ResponseHead head;
		std::size_t consumed = 0;
		const ParseResult result = parse_response_head(std::string_view(r.buf).substr(r.pos), kMaxHeadBytes, head, consumed);
		if (result == ParseResult::Bad) throw FetchError("invalid response");
		if (result == ParseResult::Incomplete) {
			if (!r.fill()) throw FetchError("connection closed before response");
			continue;
		}
		r.pos += consumed;
		if (head.status >= 100 && head.status < 200) continue;	//100 Continue などは読み飛ばす
		return head;
	}
}

void read_chunked(Reader& r, std::size_t limit, std::string& out) {
	for (;;) {
		const std::string line = r.read_line();
		const std::string_view size_text = trim(std::string_view(line).substr(0, line.find(';')));
		if (size_text.empty() || size_text.size() > 8) throw FetchError("invalid chunk");
		std::size_t size = 0;
		for (const char c : size_text) {
			int v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else throw FetchError("invalid chunk");
			size = size * 16 + static_cast<std::size_t>(v);
		}
		if (size == 0) {
			while (!r.read_line().empty()) {}	//トレーラは捨てる
			return;
		}
		if (out.size() + size > limit) throw FetchError("response too large");
		r.read_exact(size, out);
		if (!r.read_line().empty()) throw FetchError("invalid chunk");
	}
}

std::string build_request(const Url& url, const FetchRequest& request) {
	for (const char c : request.method) {
		if (c < 'A' || c > 'Z') throw std::invalid_argument("invalid method");
	}
	std::string out = request.method + " " + url.target + " HTTP/1.1\r\nHost: " + url.host_header() + "\r\nConnection: close\r\n";
	for (const auto& kv : request.headers) {
		if (!is_valid_header_name(kv.first) || !is_valid_header_value(kv.second)) throw std::invalid_argument("invalid request header " + kv.first);
		const std::string name = to_lower(kv.first);
		if (name == "host" || name == "connection" || name == "content-length" || name == "transfer-encoding") continue;
		out += kv.first + ": " + kv.second + "\r\n";
	}
	if (!request.body.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") {
		out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
	}
	out += "\r\n";
	out += request.body;
	return out;
}

}

FetchResponse fetch(const std::string& url_text, const FetchRequest& request) {
	Url url;
	if (!parse_url(url_text, url) || (url.scheme != "http" && url.scheme != "https")) throw std::invalid_argument("invalid url");
	if (!url.userinfo.empty()) throw std::invalid_argument("url must not contain credentials");
	const std::string bytes = build_request(url, request);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeout_ms);
	Channel channel(url.host, url.port, url.scheme == "https", request.ca_file, deadline);
	channel.write_all(bytes);

	Reader reader{ channel, std::string() };
	const ResponseHead head = read_head(reader);
	if (head.status >= 300 && head.status < 400 && head.status != 304) throw FetchError("redirect not allowed (" + std::to_string(head.status) + ")");

	FetchResponse res;
	res.status = head.status;
	res.headers = head.headers;
	if (request.method == "HEAD" || head.status == 204 || head.status == 304) return res;

	const std::size_t limit = request.max_response_bytes;
	std::uint64_t length = 0;
	if (header_has_token(head.headers.get("transfer-encoding"), "chunked")) {
		read_chunked(reader, limit, res.body);
	} else if (head.headers.find("content-length")) {
		if (!parse_content_length(head.headers, length)) throw FetchError("invalid response");
		if (length > limit) throw FetchError("response too large");
		reader.read_exact(static_cast<std::size_t>(length), res.body);
	} else {
		//長さの指定が無ければ、相手が閉じるまで
		do {
			res.body.append(reader.buf, reader.pos, std::string::npos);
			reader.pos = reader.buf.size();
			if (res.body.size() > limit) throw FetchError("response too large");
		} while (reader.fill());
	}
	return res;
}

Json fetch_json(const std::string& url_text, const JsonRequest& request) {
	FetchRequest req;
	req.timeout_ms = request.timeout_ms;
	req.ca_file = request.ca_file;
	req.headers.emplace_back("Accept", "application/json");
	for (const auto& kv : request.headers) req.headers.push_back(kv);
	if (request.body) {
		req.headers.emplace_back("Content-Type", "application/json");
		req.body = request.body->dump();
	}
	req.method = !request.method.empty() ? request.method : (request.body ? "POST" : "GET");

	const FetchResponse res = fetch(url_text, req);
	if (!res.ok()) {
		Url url;
		parse_url(url_text, url);
		throw HttpError(url.host_header() + " responded " + std::to_string(res.status), res.status, res.body.substr(0, 500));
	}
	if (res.body.empty()) return Json(nullptr);
	try {
		return Json::parse(res.body);
	} catch (const JsonError& ex) {
		throw FetchError(std::string("response is not json: ") + ex.what());
	}
}

std::string path_segment(const std::string& value, const std::string& name) {
	bool ok = !value.empty() && value.size() <= 128;
	for (const char c : value) {
		const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
		ok = ok && allowed;
	}
	if (!ok) throw std::invalid_argument("invalid " + name);
	return percent_encode_component(value);
}

}
