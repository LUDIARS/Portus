#include "portus/log.hpp"

#include "portus/util.hpp"

namespace portus {

LogLevel parse_level(std::string_view text, LogLevel fallback) {
	const std::string v = to_lower(text);
	if (v == "debug") return LogLevel::Debug;
	if (v == "info") return LogLevel::Info;
	if (v == "warn") return LogLevel::Warn;
	if (v == "error") return LogLevel::Error;
	return fallback;
}

const char* level_name(LogLevel level) {
	switch (level) {
	case LogLevel::Debug: return "debug";
	case LogLevel::Info: return "info";
	case LogLevel::Warn: return "warn";
	case LogLevel::Error: return "error";
	}
	return "info";
}

//ログに残してはいけない値を持つキー (小文字で比較)
static bool is_secret_key(std::string_view key) {
	static const char* const keys[] = {
		"session", "token", "password", "pass", "auth", "authorization", "p256dh", "secret", "apikey", "api_key", "key",
		"cookie", "set-cookie", "x-api-key", "access_token", "refresh_token", "privatekey", "private_key",
	};
	const std::string k = to_lower(key);
	for (const char* s : keys) {
		if (k == s) return true;
	}
	return false;
}

Json redact(const Json& value, int depth) {
	if (!value.is_array() && !value.is_object()) return value;
	if (depth >= 6) return Json("[Depth]");

	if (value.is_array()) {
		Json out = Json::array();
		for (const Json& item : value.items()) out.push_back(redact(item, depth + 1));
		return out;
	}
	Json::Object out;
	out.reserve(value.members().size());
	for (const Json::Member& m : value.members()) {
		out.emplace_back(m.first, is_secret_key(m.first) ? Json("[REDACTED]") : redact(m.second, depth + 1));
	}
	return Json::from_members(std::move(out));
}

}
