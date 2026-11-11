#include "portus/env.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "portus/util.hpp"

namespace portus {

static bool valid_key(std::string_view key) {
	if (key.empty() || (key[0] >= '0' && key[0] <= '9')) return false;
	for (const char c : key) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) return false;
	}
	return true;
}

static std::string parse_value(std::string_view value) {
	if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'')) {
		const char quote = value[0];
		const std::size_t end = value.find(quote, 1);
		if (end != std::string_view::npos) {
			const std::string_view inner = value.substr(1, end - 1);
			if (quote == '\'') return std::string(inner);

			std::string out;
			for (std::size_t i = 0; i < inner.size(); ++i) {
				if (inner[i] == '\\' && i + 1 < inner.size() && inner[i + 1] == 'n') {
					out.push_back('\n');
					++i;
				} else {
					out.push_back(inner[i]);
				}
			}
			return out;
		}
	}

	//クォート無しは行内コメント (空白 + #) を落とす
	for (std::size_t i = 0; i + 1 < value.size(); ++i) {
		if ((value[i] == ' ' || value[i] == '\t') && value[i + 1] == '#') {
			value = value.substr(0, i);
			break;
		}
	}
	return std::string(trim(value));
}

std::vector<std::pair<std::string, std::string>> parse_env(std::string_view text) {
	std::vector<std::pair<std::string, std::string>> out;
	for (const std::string& raw : split(text, '\n')) {
		std::string_view line = trim(raw);
		if (line.empty() || line[0] == '#') continue;
		if (starts_with(line, "export ")) line = trim(line.substr(7));

		const std::size_t eq = line.find('=');
		if (eq == std::string_view::npos || eq == 0) continue;
		const std::string_view key = trim(line.substr(0, eq));
		if (!valid_key(key)) continue;

		std::string value = parse_value(trim(line.substr(eq + 1)));
		bool replaced = false;
		for (auto& kv : out) {
			if (kv.first == key) {
				kv.second = value;
				replaced = true;
			}
		}
		if (!replaced) out.emplace_back(std::string(key), std::move(value));
	}
	return out;
}

std::string get_env(const char* name, const std::string& fallback) {
	const char* v = std::getenv(name);
	return v ? std::string(v) : fallback;
}

void load_env(const std::string& file) {
	std::ifstream in(file, std::ios::binary);
	if (!in) return;
	std::stringstream ss;
	ss << in.rdbuf();

	for (const auto& kv : parse_env(ss.str())) {
		if (std::getenv(kv.first.c_str())) continue;
#ifdef _WIN32
		_putenv_s(kv.first.c_str(), kv.second.c_str());
#else
		setenv(kv.first.c_str(), kv.second.c_str(), 0);
#endif
	}
}

}
