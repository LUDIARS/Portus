#include "portus/crypto.hpp"

namespace portus {

std::string hex(std::string_view data) {
	static const char* digits = "0123456789abcdef";
	std::string out;
	out.reserve(data.size() * 2);
	for (const char ch : data) {
		const unsigned char c = static_cast<unsigned char>(ch);
		out.push_back(digits[c >> 4]);
		out.push_back(digits[c & 0x0F]);
	}
	return out;
}

static std::string encode(std::string_view data, const char* table, bool pad) {
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 2 < data.size(); i += 3) {
		const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
		out.push_back(table[(v >> 18) & 63]);
		out.push_back(table[(v >> 12) & 63]);
		out.push_back(table[(v >> 6) & 63]);
		out.push_back(table[v & 63]);
	}
	const std::size_t rest = data.size() - i;
	if (rest == 1) {
		const unsigned v = static_cast<unsigned char>(data[i]) << 16;
		out.push_back(table[(v >> 18) & 63]);
		out.push_back(table[(v >> 12) & 63]);
		if (pad) out += "==";
	} else if (rest == 2) {
		const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
		out.push_back(table[(v >> 18) & 63]);
		out.push_back(table[(v >> 12) & 63]);
		out.push_back(table[(v >> 6) & 63]);
		if (pad) out.push_back('=');
	}
	return out;
}

std::string base64_encode(std::string_view data) {
	return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

std::string base64url_encode(std::string_view data) {
	return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

static int sextet(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

bool base64_decode(std::string_view text, std::string& out) {
	out.clear();
	while (!text.empty() && text.back() == '=') text.remove_suffix(1);
	if (text.size() % 4 == 1) return false;

	unsigned acc = 0;
	int bits = 0;
	for (const char c : text) {
		const int v = sextet(c);
		if (v < 0) return false;
		acc = (acc << 6) | static_cast<unsigned>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<char>((acc >> bits) & 0xFF));
		}
	}
	return true;
}

}
