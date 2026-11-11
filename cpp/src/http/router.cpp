#include "portus/http.hpp"

#include <cstdlib>
#include <stdexcept>

#include "portus/util.hpp"

namespace portus {

static bool is_word(std::string_view s) {
	if (s.empty()) return false;
	for (const char c : s) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) return false;
	}
	return true;
}

//"モジュール#関数" は [A-Za-z0-9_] だけ
static void check_handler_ref(const std::string& ref) {
	const std::size_t hash = ref.find('#');
	if (hash == std::string::npos || !is_word(std::string_view(ref).substr(0, hash)) || !is_word(std::string_view(ref).substr(hash + 1))) {
		throw std::invalid_argument("invalid handler \"" + ref + "\" (expected \"module#function\")");
	}
}

RouteDef::RouteDef(std::string method_, std::string path_, Handler handler_, Json meta_, std::string note_)
	: method(std::move(method_)), path(std::move(path_)), handler(std::move(handler_)), meta(std::move(meta_)), note(std::move(note_)) {}

RouteDef::RouteDef(std::string method_, std::string path_, const char* handler_ref_, Json meta_, std::string note_)
	: method(std::move(method_)), path(std::move(path_)), handler_ref(handler_ref_), meta(std::move(meta_)), note(std::move(note_)) {}

RouteDef::RouteDef(std::string method_, std::string path_, std::string handler_ref_, Json meta_, std::string note_)
	: method(std::move(method_)), path(std::move(path_)), handler_ref(std::move(handler_ref_)), meta(std::move(meta_)), note(std::move(note_)) {}

static std::vector<std::string> split_path(std::string_view path) {
	std::vector<std::string> out;
	for (std::string& part : split(path, '/')) {
		if (!part.empty()) out.push_back(std::move(part));
	}
	return out;
}

//-?[0-9]+(\.[0-9]+)?
static bool is_number_value(std::string_view v) {
	std::size_t i = 0;
	if (i < v.size() && v[i] == '-') ++i;
	const std::size_t int_start = i;
	while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
	if (i == int_start) return false;
	if (i == v.size()) return true;
	if (v[i] != '.') return false;
	const std::size_t frac_start = ++i;
	while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
	return i > frac_start && i == v.size();
}

Router::Router(std::vector<RouteDef> defs) {
	for (RouteDef& def : defs) {
		if (def.method.empty()) throw std::invalid_argument("route method is empty");
		for (const char c : def.method) {
			if (c < 'A' || c > 'Z') throw std::invalid_argument("invalid method \"" + def.method + "\"");
		}
		if (!def.handler) check_handler_ref(def.handler_ref);

		Compiled route;
		for (const std::string& seg : split_path(def.path)) {
			Segment s;
			s.text = seg;
			if (seg.size() >= 3 && seg.front() == '{' && seg.back() == '}') {
				std::string name = seg.substr(1, seg.size() - 2);
				bool number = false;
				const std::size_t colon = name.find(':');
				bool typed_ok = true;
				if (colon != std::string::npos) {
					const std::string type = name.substr(colon + 1);
					typed_ok = type == "string" || type == "number";
					number = type == "number";
					name = name.substr(0, colon);
				}
				if (typed_ok && is_word(name) && !(name[0] >= '0' && name[0] <= '9')) {
					s.is_param = true;
					s.is_number = number;
					s.text = name;
				}
			}
			route.segments.push_back(std::move(s));
		}
		route.def = std::make_shared<const RouteDef>(std::move(def));
		routes_.push_back(std::move(route));
	}
}

std::optional<RouteMatch> Router::match(std::string_view method, std::string_view url) const {
	const std::size_t q = url.find('?');
	const std::vector<std::string> raw = split_path(url.substr(0, q));

	//区切ってからデコードする (%2F を区切りとして扱わない)。不正な % 列・UTF-8 はどのルートにも一致させない
	std::vector<std::string> parts(raw.size());
	for (std::size_t i = 0; i < raw.size(); ++i) {
		if (!percent_decode_strict(raw[i], parts[i])) return std::nullopt;
	}

	for (const Compiled& route : routes_) {
		if (route.def->method != method || route.segments.size() != parts.size()) continue;

		Json params = Json::object();
		bool ok = true;
		for (std::size_t i = 0; i < parts.size() && ok; ++i) {
			const Segment& seg = route.segments[i];
			if (!seg.is_param) ok = seg.text == parts[i];
			else if (!seg.is_number) params.set(seg.text, Json(parts[i]));
			else if (is_number_value(parts[i])) params.set(seg.text, Json(std::strtod(parts[i].c_str(), nullptr)));
			else ok = false;
		}
		if (!ok) continue;

		RouteMatch m;
		m.def = route.def;
		m.query = q == std::string_view::npos ? Json::object() : parse_query_string(url.substr(q + 1));
		for (const Json::Member& p : params.members()) m.query.set(p.first, p.second);
		m.params = std::move(params);
		return m;
	}
	return std::nullopt;
}

std::vector<RouteDef> Router::list() const {
	std::vector<RouteDef> out;
	for (const Compiled& route : routes_) out.push_back(*route.def);
	return out;
}

}
