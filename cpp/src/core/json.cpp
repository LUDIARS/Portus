#include "portus/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace portus {

Json Json::object(std::initializer_list<Member> members) {
	Json j;
	j.v_ = Object(members);
	return j;
}

Json Json::array(std::initializer_list<Json> items) {
	Json j;
	j.v_ = Array(items);
	return j;
}

Json Json::from_members(Object members) {
	Json j;
	j.v_ = std::move(members);
	return j;
}

bool Json::as_bool(bool fallback) const {
	const bool* p = std::get_if<bool>(&v_);
	return p ? *p : fallback;
}

double Json::as_number(double fallback) const {
	const double* p = std::get_if<double>(&v_);
	return p ? *p : fallback;
}

const std::string& Json::as_string() const {
	static const std::string empty;
	const std::string* p = std::get_if<std::string>(&v_);
	return p ? *p : empty;
}

const Json::Array& Json::items() const {
	static const Array empty;
	const Array* p = std::get_if<Array>(&v_);
	return p ? *p : empty;
}

const Json::Object& Json::members() const {
	static const Object empty;
	const Object* p = std::get_if<Object>(&v_);
	return p ? *p : empty;
}

const Json* Json::find(std::string_view key) const {
	for (const Member& m : members()) {
		if (m.first == key) return &m.second;
	}
	return nullptr;
}

const Json& Json::at(std::string_view key) const {
	static const Json null;
	const Json* p = find(key);
	return p ? *p : null;
}

void Json::set(std::string key, Json value) {
	if (!is_object()) v_ = Object();
	Object& obj = std::get<Object>(v_);
	for (Member& m : obj) {
		if (m.first == key) {
			m.second = std::move(value);
			return;
		}
	}
	obj.emplace_back(std::move(key), std::move(value));
}

void Json::push_back(Json value) {
	if (!is_array()) v_ = Array();
	std::get<Array>(v_).push_back(std::move(value));
}

std::string json_quote(std::string_view s) {
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back('"');
	for (const char ch : s) {
		const unsigned char c = static_cast<unsigned char>(ch);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20 || c == 0x7F) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(ch);
			}
		}
	}
	out.push_back('"');
	return out;
}

static void dump_number(double d, std::string& out) {
	if (!std::isfinite(d)) {
		out += "null";
		return;
	}
	char buf[32];
	if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
	else std::snprintf(buf, sizeof(buf), "%.17g", d);
	out += buf;
}

static void dump_value(const Json& j, std::string& out) {
	if (j.is_null()) out += "null";
	else if (j.is_bool()) out += j.as_bool() ? "true" : "false";
	else if (j.is_number()) dump_number(j.as_number(), out);
	else if (j.is_string()) out += json_quote(j.as_string());
	else if (j.is_array()) {
		out.push_back('[');
		bool first = true;
		for (const Json& item : j.items()) {
			if (!first) out.push_back(',');
			first = false;
			dump_value(item, out);
		}
		out.push_back(']');
	} else {
		out.push_back('{');
		bool first = true;
		for (const Json::Member& m : j.members()) {
			if (!first) out.push_back(',');
			first = false;
			out += json_quote(m.first);
			out.push_back(':');
			dump_value(m.second, out);
		}
		out.push_back('}');
	}
}

std::string Json::dump() const {
	std::string out;
	dump_value(*this, out);
	return out;
}

namespace {

class Parser {
public:
	Parser(std::string_view text, std::size_t max_depth) : s_(text), max_depth_(max_depth) {}

	Json run() {
		Json value = parse_value(0);
		skip_space();
		if (pos_ != s_.size()) fail("trailing characters");
		return value;
	}

private:
	[[noreturn]] void fail(const char* what) const {
		throw JsonError(std::string("invalid json: ") + what + " at " + std::to_string(pos_));
	}

	void skip_space() {
		while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
	}

	bool consume(std::string_view word) {
		if (s_.compare(pos_, word.size(), word) != 0) return false;
		pos_ += word.size();
		return true;
	}

	Json parse_value(std::size_t depth) {
		if (depth > max_depth_) fail("too deep");
		skip_space();
		if (pos_ >= s_.size()) fail("unexpected end");

		const char c = s_[pos_];
		if (c == '{') return parse_object(depth);
		if (c == '[') return parse_array(depth);
		if (c == '"') return Json(parse_string());
		if (consume("true")) return Json(true);
		if (consume("false")) return Json(false);
		if (consume("null")) return Json(nullptr);
		return parse_number();
	}

	//同じキーは後勝ち。キーが多いときは索引を使い、巨大なオブジェクトで 2 乗時間にならないようにする
	Json parse_object(std::size_t depth) {
		Json::Object members;
		std::unordered_map<std::string, std::size_t> index;
		++pos_;
		skip_space();
		if (pos_ < s_.size() && s_[pos_] == '}') {
			++pos_;
			return Json::object();
		}
		for (;;) {
			skip_space();
			if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected key");
			std::string key = parse_string();
			skip_space();
			if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':'");
			++pos_;
			Json value = parse_value(depth + 1);

			const auto found = index.find(key);
			if (found != index.end()) {
				members[found->second].second = std::move(value);
			} else {
				index.emplace(key, members.size());
				members.emplace_back(std::move(key), std::move(value));
			}

			skip_space();
			if (pos_ >= s_.size()) fail("unexpected end");
			if (s_[pos_] == ',') { ++pos_; continue; }
			if (s_[pos_] == '}') { ++pos_; return Json::from_members(std::move(members)); }
			fail("expected ',' or '}'");
		}
	}

	Json parse_array(std::size_t depth) {
		Json out = Json::array();
		++pos_;
		skip_space();
		if (pos_ < s_.size() && s_[pos_] == ']') {
			++pos_;
			return out;
		}
		for (;;) {
			out.push_back(parse_value(depth + 1));
			skip_space();
			if (pos_ >= s_.size()) fail("unexpected end");
			if (s_[pos_] == ',') { ++pos_; continue; }
			if (s_[pos_] == ']') { ++pos_; return out; }
			fail("expected ',' or ']'");
		}
	}

	unsigned parse_hex4() {
		if (pos_ + 4 > s_.size()) fail("bad \\u escape");
		unsigned v = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = s_[pos_++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
			else fail("bad \\u escape");
		}
		return v;
	}

	static void append_utf8(std::string& out, unsigned cp) {
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	std::string parse_string() {
		std::string out;
		++pos_;
		for (;;) {
			if (pos_ >= s_.size()) fail("unterminated string");
			const char c = s_[pos_++];
			if (c == '"') return out;
			if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (pos_ >= s_.size()) fail("unterminated string");
			const char e = s_[pos_++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				unsigned cp = parse_hex4();
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					//後ろが低位サロゲートのときだけ対として読む。そうでなければ後ろのエスケープは
					//読み戻して普通に解釈させる (JSON.parse は両方の符号単位を残すので、捨てると文字が消える)
					const std::size_t back = pos_;
					unsigned lo = 0;
					if (consume("\\u")) lo = parse_hex4();
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					} else {
						pos_ = back;
						cp = 0xFFFD;
					}
				} else if (cp >= 0xD800 && cp <= 0xDFFF) {
					cp = 0xFFFD;	//対にならないサロゲート
				}
				append_utf8(out, cp);
				break;
			}
			default: fail("bad escape");
			}
		}
	}

	Json parse_number() {
		const std::size_t start = pos_;
		if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
		const std::size_t digits = pos_;
		while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
		if (pos_ == digits) fail("unexpected character");
		if (s_[digits] == '0' && pos_ - digits > 1) fail("leading zero");
		if (pos_ < s_.size() && s_[pos_] == '.') {
			++pos_;
			const std::size_t frac = pos_;
			while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
			if (pos_ == frac) fail("bad number");
		}
		if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
			++pos_;
			if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
			const std::size_t exp = pos_;
			while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
			if (pos_ == exp) fail("bad number");
		}
		const std::string text(s_.substr(start, pos_ - start));
		return Json(std::strtod(text.c_str(), nullptr));
	}

	std::string_view s_;
	std::size_t max_depth_;
	std::size_t pos_ = 0;
};

}

Json Json::parse(std::string_view text, std::size_t max_depth) {
	return Parser(text, max_depth).run();
}

}
