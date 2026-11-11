#include "portus/http.hpp"

#include <string_view>
#include <unordered_map>

#include "portus/util.hpp"

namespace portus {

namespace {

//キーの索引を持ってオブジェクトを組み立てる。Json::set は線形探索なので、キーが大量の入力を 1 つずつ set すると 2 乗時間になる
//同じキーは後勝ちで、最初に現れた位置に置く (Json::set と同じ)
class ObjectBuilder {
public:
	void set(std::string key, Json value) {
		const auto found = index_.find(key);
		if (found != index_.end()) {
			members_[found->second].second = std::move(value);
			return;
		}
		index_.emplace(key, members_.size());
		members_.emplace_back(std::move(key), std::move(value));
	}

	Json take() { return Json::from_members(std::move(members_)); }

private:
	Json::Object members_;
	std::unordered_map<std::string, std::size_t> index_;
};

//src のキーは重複しない (Json::parse と parse_query_string が後勝ちでまとめている) ので、索引は dst の分だけでよい
void assign_safe(Json& dst, const Json& src, const Json& skip) {
	if (src.members().empty()) return;
	Json::Object out = dst.members();
	out.reserve(out.size() + src.members().size());	//以後は伸ばしても移動しないので、索引はキーを指したままでよい
	std::unordered_map<std::string_view, std::size_t> existing;
	for (std::size_t i = 0; i < out.size(); ++i) existing.emplace(out[i].first, i);
	for (const Json::Member& m : src.members()) {
		if (!is_safe_key(m.first) || skip.find(m.first)) continue;
		const auto found = existing.find(m.first);
		if (found != existing.end()) out[found->second].second = m.second;
		else out.emplace_back(m.first, m.second);
	}
	dst = Json::from_members(std::move(out));
}

}

Json parse_query_string(std::string_view qs) {
	ObjectBuilder out;
	if (!qs.empty() && qs[0] == '?') qs.remove_prefix(1);
	for (const std::string& pair : split(qs, '&')) {
		if (pair.empty()) continue;
		const std::size_t eq = pair.find('=');
		std::string key = percent_decode_form(std::string_view(pair).substr(0, eq));
		const std::string value = eq == std::string::npos ? "" : percent_decode_form(std::string_view(pair).substr(eq + 1));
		if (is_safe_key(key)) out.set(std::move(key), Json(value));
	}
	return out.take();
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
