#include "portus/http.hpp"

#include "portus/util.hpp"

namespace portus {

Json parse_query_string(std::string_view qs) {
	Json out = Json::object();
	if (!qs.empty() && qs[0] == '?') qs.remove_prefix(1);
	for (const std::string& pair : split(qs, '&')) {
		if (pair.empty()) continue;
		const std::size_t eq = pair.find('=');
		const std::string key = percent_decode_form(std::string_view(pair).substr(0, eq));
		const std::string value = eq == std::string::npos ? "" : percent_decode_form(std::string_view(pair).substr(eq + 1));
		if (is_safe_key(key)) out.set(key, Json(value));
	}
	return out;
}

static void assign_safe(Json& dst, const Json& src, const Json& skip) {
	for (const Json::Member& m : src.members()) {
		if (!is_safe_key(m.first) || skip.find(m.first)) continue;
		dst.set(m.first, m.second);
	}
}

void merge_body(Json& dst, std::string_view text, const Json& skip) {
	if (text.empty()) return;
	try {
		//オブジェクト以外 (配列・数値など) は足すキーが無い。生のボディは ctx.body で読める
		assign_safe(dst, Json::parse(text), skip);
		return;
	} catch (const JsonError&) {
		//JSON でなければフォーム形式として読む
	}
	assign_safe(dst, parse_query_string(text), skip);
}

}
