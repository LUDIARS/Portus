#pragma once
//JSON 値。解析は深さに上限を持ち、オブジェクトは書いた順を保つ
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace portus {

class JsonError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class Json {
public:
	using Array = std::vector<Json>;
	using Member = std::pair<std::string, Json>;
	using Object = std::vector<Member>;

	Json() : v_(nullptr) {}
	Json(std::nullptr_t) : v_(nullptr) {}
	Json(bool b) : v_(b) {}
	Json(const char* s) : v_(std::string(s)) {}
	Json(std::string s) : v_(std::move(s)) {}
	Json(std::string_view s) : v_(std::string(s)) {}
	Json(Array a) : v_(std::move(a)) {}
	template <class T, class = std::enable_if_t<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>>
	Json(T n) : v_(static_cast<double>(n)) {}

	static Json object(std::initializer_list<Member> members = {});
	static Json array(std::initializer_list<Json> items = {});
	static Json from_members(Object members);	//キーの重複が無いことは呼び出し側が保証する

	bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
	bool is_bool() const { return std::holds_alternative<bool>(v_); }
	bool is_number() const { return std::holds_alternative<double>(v_); }
	bool is_string() const { return std::holds_alternative<std::string>(v_); }
	bool is_array() const { return std::holds_alternative<Array>(v_); }
	bool is_object() const { return std::holds_alternative<Object>(v_); }

	//型が違うときは既定値を返す (外部入力を読むときに例外で落とさない)
	bool as_bool(bool fallback = false) const;
	double as_number(double fallback = 0) const;
	const std::string& as_string() const;	//文字列でなければ空文字列
	const Array& items() const;				//配列でなければ空
	const Object& members() const;			//オブジェクトでなければ空

	const Json* find(std::string_view key) const;	//無ければ nullptr
	const Json& at(std::string_view key) const;		//無ければ null
	void set(std::string key, Json value);			//オブジェクトでなければオブジェクトにする。同じキーは置き換え
	void push_back(Json value);						//配列でなければ配列にする

	std::string dump() const;
	static Json parse(std::string_view text, std::size_t max_depth = 64);	//失敗時 JsonError

	bool operator==(const Json& other) const { return v_ == other.v_; }
	bool operator!=(const Json& other) const { return !(v_ == other.v_); }

private:
	std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;
};

//文字列を JSON の文字列リテラル (引用符付き) にする
std::string json_quote(std::string_view s);

}
