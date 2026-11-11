#include "portus/http.hpp"
#include "test.hpp"

using namespace portus;

static Reply noop(RequestContext&) {
	return Reply::no_content();
}

static Router make_router() {
	return Router({
		{ "GET", "/stat", "stat#check" },
		{ "GET", "/user/{id:number}", "user#byId" },
		{ "GET", "/user/{hash}", "user#byHash" },
		{ "GET", "/games/{id:number}", noop },
		{ "POST", "/login", "user#login" },
	});
}

TEST(router_types, "ルータ: 数値変数と文字列変数を型で振り分ける") {
	const Router router = make_router();
	const auto by_id = router.match("GET", "/user/1030");
	CHECK(by_id.has_value());
	CHECK_EQ(by_id->def->handler_ref, "user#byId");
	CHECK_EQ(by_id->params.at("id").as_number(), 1030);
	CHECK_EQ(by_id->query.at("id").as_number(), 1030);

	const auto by_hash = router.match("GET", "/user/423c1220-c2cf");
	CHECK_EQ(by_hash->def->handler_ref, "user#byHash");
	CHECK_EQ(by_hash->params.at("hash").as_string(), "423c1220-c2cf");

	CHECK_EQ(router.match("GET", "/user/-1.5")->params.at("id").as_number(), -1.5);
	CHECK(!router.match("GET", "/games/abc").has_value());
	CHECK(!router.match("GET", "/games/1e5").has_value());
	CHECK(!router.match("GET", "/games/1.").has_value());
}

TEST(router_query, "ルータ: 末尾スラッシュとクエリを扱える") {
	const auto m = make_router().match("GET", "/stat/?a=1&b=%E3%81%82&c=x+y");
	CHECK(m.has_value());
	CHECK_EQ(m->def->handler_ref, "stat#check");
	CHECK_EQ(m->query.at("a").as_string(), "1");
	CHECK_EQ(m->query.at("b").as_string(), "あ");
	CHECK_EQ(m->query.at("c").as_string(), "x y");
}

TEST(router_no_fallback, "ルータ: 表に無いパス・メソッドは一致しない (親ルートへフォールバックしない)") {
	const Router router = make_router();
	CHECK(!router.match("GET", "/stat/unknown").has_value());
	CHECK(!router.match("POST", "/stat").has_value());
	CHECK(!router.match("GET", "/../user/1").has_value());
	CHECK(!router.match("GET", "/user/%E0%A4%A").has_value());
	CHECK(!router.match("GET", "/user%2F1").has_value());	//%2F は区切りにならない
	CHECK_EQ(router.list().size(), 5u);
}

TEST(router_invalid_defs, "ルータ: 不正なハンドラ名・メソッドは生成時に弾く") {
	CHECK_THROWS(Router({ { "GET", "/x", "../x#run" } }));
	CHECK_THROWS(Router({ { "GET", "/x", "x" } }));
	CHECK_THROWS(Router({ { "get", "/x", "x#run" } }));
}

TEST(body_merge, "ボディ: JSON / フォームを取り込み、__proto__ などのキーは捨てる") {
	Json query = Json::object();
	merge_body(query, "{\"__proto__\":{\"polluted\":1},\"constructor\":1,\"prototype\":2,\"a\":2}");
	CHECK_EQ(query.at("a").as_number(), 2);
	CHECK_EQ(query.members().size(), 1u);

	Json form = Json::object();
	merge_body(form, "__proto__=1&x=%E3%81%82");
	CHECK_EQ(form.at("x").as_string(), "あ");
	CHECK_EQ(form.members().size(), 1u);

	//JSON の配列・数値はキーを持たないので何も足さない
	Json other = Json::object();
	merge_body(other, "[1,2,3]");
	merge_body(other, "42");
	CHECK_EQ(other.members().size(), 0u);

	const auto m = make_router().match("GET", "/stat?__proto__=x&ok=1");
	CHECK_EQ(m->query.members().size(), 1u);
	CHECK_EQ(m->query.at("ok").as_string(), "1");
}

TEST(body_cannot_override_path, "ボディ: パス変数と同じ名前のキーで上書きできない") {
	auto m = make_router().match("GET", "/user/7?id=999");
	CHECK_EQ(m->query.at("id").as_number(), 7);
	merge_body(m->query, "{\"id\":999,\"name\":\"x\"}", m->params);
	CHECK_EQ(m->query.at("id").as_number(), 7);
	CHECK_EQ(m->query.at("name").as_string(), "x");
}

TEST(request_head_parse, "HTTP: 要求ヘッダの解析と不正な形の拒否") {
	RequestHead head;
	std::size_t consumed = 0;
	const std::string ok = "POST /a?b=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nX-Token:  abc \r\n\r\nxyz";
	CHECK(parse_request_head(ok, 1024, head, consumed) == ParseResult::Ok);
	CHECK_EQ(head.method, "POST");
	CHECK_EQ(head.target, "/a?b=1");
	CHECK_EQ(head.headers.get("x-token"), "abc");
	CHECK_EQ(consumed, ok.size() - 3);

	CHECK(parse_request_head("GET / HTTP/1.1\r\nHost: x\r\n", 1024, head, consumed) == ParseResult::Incomplete);
	const char* bad[] = {
		"GET / HTTP/2.0\r\n\r\n",
		"GET http://evil/ HTTP/1.1\r\n\r\n",
		"get / HTTP/1.1\r\n\r\n",
		"GET /a b HTTP/1.1\r\n\r\n",
		"GET / HTTP/1.1\r\nHost : x\r\n\r\n",			//名前の後ろの空白
		"GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n",	//行の折り返し
		"GET / HTTP/1.1\r\nNoColon\r\n\r\n",
		"GET / HTTP/1.1\r\nA: x\x01y\r\n\r\n",
	};
	for (const char* text : bad) CHECK(parse_request_head(text, 1024, head, consumed) == ParseResult::Bad);
	CHECK(parse_request_head(std::string(2000, 'a'), 1024, head, consumed) == ParseResult::Bad);

	std::uint64_t length = 0;
	Headers h;
	h.add("Content-Length", "5");
	h.add("Content-Length", "6");
	CHECK(!parse_content_length(h, length));	//食い違う重複
	Headers h2;
	h2.add("Content-Length", "+5");
	CHECK(!parse_content_length(h2, length));
}
