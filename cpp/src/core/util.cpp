#include "portus/util.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace portus {

std::string to_lower(std::string_view s) {
	std::string out(s);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return out;
}

std::string_view trim(std::string_view s) {
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!s.empty() && space(s.front())) s.remove_prefix(1);
	while (!s.empty() && space(s.back())) s.remove_suffix(1);
	return s;
}

std::vector<std::string> split(std::string_view s, char sep) {
	std::vector<std::string> out;
	std::size_t start = 0;
	for (;;) {
		const std::size_t pos = s.find(sep, start);
		if (pos == std::string_view::npos) {
			out.emplace_back(s.substr(start));
			return out;
		}
		out.emplace_back(s.substr(start, pos - start));
		start = pos + 1;
	}
}

bool starts_with(std::string_view s, std::string_view prefix) {
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool utf8_valid(std::string_view s) {
	const std::size_t n = s.size();
	std::size_t i = 0;
	while (i < n) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x80) {
			++i;
			continue;
		}
		int extra;
		std::uint32_t cp;
		std::uint32_t min;
		if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
		else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
		else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
		else return false;

		if (i + extra >= n) return false;
		for (int k = 1; k <= extra; ++k) {
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		i += extra + 1;
	}
	return true;
}

static int hex_value(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool percent_decode_strict(std::string_view s, std::string& out) {
	out.clear();
	out.reserve(s.size());
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '%') {
			out.push_back(s[i]);
			continue;
		}
		if (i + 2 >= s.size()) return false;
		const int hi = hex_value(s[i + 1]);
		const int lo = hex_value(s[i + 2]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<char>((hi << 4) | lo));
		i += 2;
	}
	return utf8_valid(out);
}

std::string percent_decode_form(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out.push_back(' ');
		} else if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
			out.push_back(static_cast<char>((hex_value(s[i + 1]) << 4) | hex_value(s[i + 2])));
			i += 2;
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

std::string percent_encode_component(std::string_view s) {
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (const char ch : s) {
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
		if (keep) {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0F]);
		}
	}
	return out;
}

bool is_safe_key(std::string_view key) {
	return key != "__proto__" && key != "constructor" && key != "prototype";
}

std::int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_time(std::int64_t epoch_ms) {
	const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
	struct tm tm;
#ifdef _WIN32
	gmtime_s(&tm, &secs);
#else
	gmtime_r(&secs, &tm);
#endif
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(epoch_ms % 1000));
	return buf;
}

}
