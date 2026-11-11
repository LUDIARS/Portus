#include "portus/ws.hpp"

#include "portus/crypto.hpp"

namespace portus {

std::string accept_key(std::string_view key) {
	return base64_encode(sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string generate_key() {
	return base64_encode(random_bytes(16));
}

//16 バイトの base64 ([A-Za-z0-9+/]{22}==)
static bool valid_key(const std::string& key) {
	if (key.size() != 24 || key[22] != '=' || key[23] != '=') return false;
	for (std::size_t i = 0; i < 22; ++i) {
		const char c = key[i];
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
		if (!ok) return false;
	}
	return true;
}

static std::string joined(const Headers& headers, std::string_view name) {
	std::string out;
	for (const auto& kv : headers.all()) {
		if (kv.first != name) continue;
		if (!out.empty()) out += ",";
		out += kv.second;
	}
	return out;
}

std::optional<std::string> validate_upgrade(const std::string& method, const Headers& headers) {
	if (method != "GET") return std::string("method must be GET");
	if (!header_has_token(joined(headers, "upgrade"), "websocket")) return std::string("missing upgrade: websocket");
	if (!header_has_token(joined(headers, "connection"), "upgrade")) return std::string("missing connection: upgrade");
	if (headers.get("sec-websocket-version") != "13") return std::string("unsupported version");
	if (headers.count("sec-websocket-key") != 1 || !valid_key(headers.get("sec-websocket-key"))) return std::string("invalid key");
	return std::nullopt;
}

std::string build_accept_response(std::string_view key) {
	return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
}

}
