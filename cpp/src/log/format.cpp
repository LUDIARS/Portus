#include "portus/log.hpp"

namespace portus {

std::string format_json(const LogRecord& rec) {
	std::string out = "{\"time\":" + json_quote(rec.time) + ",\"level\":" + json_quote(level_name(rec.level))
		+ ",\"scope\":" + json_quote(rec.scope) + ",\"msg\":" + json_quote(rec.msg);
	for (const Json::Member& m : rec.fields.members()) {
		out += "," + json_quote(m.first) + ":" + m.second.dump();
	}
	out += "}";
	return out;
}

static bool has_control(std::string_view s) {
	for (const char c : s) {
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return true;
	}
	return false;
}

//1 レコード 1 行を保つ。外から来た文字列に改行があっても、偽のログ行を作らせない
static std::string one_line(const std::string& s) {
	if (!has_control(s)) return s;
	const std::string quoted = json_quote(s);
	return quoted.substr(1, quoted.size() - 2);
}

std::string format_pretty(const LogRecord& rec) {
	std::string level = level_name(rec.level);
	for (char& c : level) c = static_cast<char>(c - 'a' + 'A');
	level.resize(5, ' ');

	std::string out = rec.time + " " + level + " [" + one_line(rec.scope) + "] " + one_line(rec.msg);
	for (const Json::Member& m : rec.fields.members()) {
		out += " " + one_line(m.first) + "=";
		out += m.second.is_string() ? one_line(m.second.as_string()) : m.second.dump();
	}
	return out;
}

}
