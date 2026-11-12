#include "portus/url.hpp"

#include "portus/util.hpp"

namespace portus {

static int default_port(const std::string& scheme) {
	if (scheme == "http" || scheme == "ws") return 80;
	if (scheme == "https" || scheme == "wss") return 443;
	if (scheme == "mqtt") return 1883;
	return 0;
}

bool parse_url(const std::string& text, Url& out) {
	for (const char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c <= 0x20 || c == 0x7F) return false;
	}
	const std::size_t scheme_end = text.find("://");
	if (scheme_end == std::string::npos || scheme_end == 0) return false;

	Url url;
	url.scheme = to_lower(text.substr(0, scheme_end));
	const std::size_t auth_start = scheme_end + 3;
	const std::size_t auth_end = text.find_first_of("/?#", auth_start);
	std::string authority = text.substr(auth_start, auth_end == std::string::npos ? std::string::npos : auth_end - auth_start);

	const std::size_t at = authority.rfind('@');
	if (at != std::string::npos) {
		url.userinfo = authority.substr(0, at);
		authority = authority.substr(at + 1);
	}

	std::string port_text;
	if (!authority.empty() && authority[0] == '[') {
		const std::size_t close = authority.find(']');
		if (close == std::string::npos) return false;
		url.host = to_lower(authority.substr(1, close - 1));
		if (close + 1 < authority.size()) {
			if (authority[close + 1] != ':') return false;
			port_text = authority.substr(close + 2);
		}
	} else {
		const std::size_t colon = authority.find(':');
		url.host = to_lower(authority.substr(0, colon));
		if (colon != std::string::npos) port_text = authority.substr(colon + 1);
	}
	if (url.host.empty()) return false;

	url.port = default_port(url.scheme);
	if (!port_text.empty()) {
		if (port_text.size() > 5) return false;
		int port = 0;
		for (const char c : port_text) {
			if (c < '0' || c > '9') return false;
			port = port * 10 + (c - '0');
		}
		if (port < 1 || port > 65535) return false;
		url.port = port;
	}
	if (url.port == 0) return false;

	std::string rest = auth_end == std::string::npos ? "" : text.substr(auth_end);
	rest = rest.substr(0, rest.find('#'));
	if (rest.empty() || rest[0] != '/') rest = "/" + rest;
	url.target = rest;

	out = url;
	return true;
}

std::string Url::host_header() const {
	const std::string name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
	return port == default_port(scheme) ? name : name + ":" + std::to_string(port);
}

std::string Url::origin() const {
	return scheme + "://" + host_header();
}

}
