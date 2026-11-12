#include "portus/crypto.hpp"
#include "portus/env.hpp"
#include "portus/event_loop.hpp"
#include "portus/id.hpp"
#include "portus/json.hpp"
#include "portus/thread_pool.hpp"
#include "portus/url.hpp"
#include "portus/util.hpp"
#include "test.hpp"

using namespace portus;

TEST(json_roundtrip, "JSON: 解析と出力が往復し、キーの順序を保つ") {
	const std::string text = "{\"b\":1,\"a\":[true,null,-2.5,\"\\u3042\\n\"],\"c\":{\"d\":\"x\"}}";
	const Json j = Json::parse(text);
	CHECK_EQ(j.at("b").as_number(), 1);
	CHECK_EQ(j.at("a").items().size(), 4u);
	CHECK_EQ(j.at("a").items()[3].as_string(), "あ\n");
	CHECK_EQ(j.at("c").at("d").as_string(), "x");
	CHECK_EQ(j.dump(), "{\"b\":1,\"a\":[true,null,-2.5,\"あ\\n\"],\"c\":{\"d\":\"x\"}}");
	CHECK(j.at("missing").is_null());
	CHECK_EQ(Json::parse("\"\\ud83d\\ude00\"").as_string(), "😀");
	//対にならないサロゲートは U+FFFD に置き換え、後ろのエスケープは捨てずに読む
	CHECK_EQ(Json::parse("\"\\ud800\\u0041\"").as_string(), "\xEF\xBF\xBD" "A");
	CHECK_EQ(Json::parse("\"\\ud800\\ud83d\\ude00\"").as_string(), "\xEF\xBF\xBD" "😀");
	CHECK_EQ(Json::parse("\"\\udc00\"").as_string(), "\xEF\xBF\xBD");
	CHECK_EQ(Json::parse("\"x\\ud800\"").as_string(), "x\xEF\xBF\xBD");
	CHECK_EQ(Json::parse("\"\\ud800x\"").as_string(), "\xEF\xBF\xBD" "x");
	CHECK_EQ(Json::parse("{\"a\":1,\"a\":2}").at("a").as_number(), 2);
	CHECK_EQ(Json(1e20).dump(), "1e+20");
	CHECK_EQ(json_quote("a\"\x01"), "\"a\\\"\\u0001\"");
}

TEST(json_reject, "JSON: 壊れた入力と深すぎる入れ子を拒否する") {
	CHECK_THROWS(Json::parse(""));
	CHECK_THROWS(Json::parse("{\"a\":1,}"));
	CHECK_THROWS(Json::parse("[1 2]"));
	CHECK_THROWS(Json::parse("01"));
	CHECK_THROWS(Json::parse("\"abc"));
	CHECK_THROWS(Json::parse("{} x"));
	CHECK_THROWS(Json::parse("nul"));
	CHECK_THROWS(Json::parse(std::string(100000, '[')));	//スタックを食い潰させない
	CHECK(Json::parse(std::string(60, '[') + std::string(60, ']')).is_array());
}

TEST(json_many_keys, "JSON: キーが大量でも 2 乗時間にならない") {
	std::string text = "{";
	for (int i = 0; i < 200000; ++i) text += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":1";
	text += "}";
	const auto started = std::chrono::steady_clock::now();
	const Json j = Json::parse(text);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK_EQ(j.members().size(), 200000u);
	CHECK(ms < 3000);
}

TEST(env_parse, ".env の解析") {
	const auto env = parse_env(
		"# comment\n"
		"A=1\r\n"
		"export B = two \n"
		"C=\"x # not comment\\ny\"\n"
		"D='raw\\n'\n"
		"E=value # comment\n"
		"1BAD=x\n"
		"A=3\n");
	const auto get = [&](const std::string& key) -> std::string {
		for (const auto& kv : env) {
			if (kv.first == key) return kv.second;
		}
		return "<none>";
	};
	CHECK_EQ(get("A"), "3");
	CHECK_EQ(get("B"), "two");
	CHECK_EQ(get("C"), "x # not comment\ny");
	CHECK_EQ(get("D"), "raw\\n");
	CHECK_EQ(get("E"), "value");
	CHECK_EQ(get("1BAD"), "<none>");
}

TEST(uuid_format, "uuidv4 の形式") {
	const std::string id = uuidv4();
	CHECK_EQ(id.size(), 36u);
	CHECK_EQ(id[14], '4');
	CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
	CHECK(id[8] == '-' && id[13] == '-' && id[18] == '-' && id[23] == '-');
	CHECK(id != uuidv4());
}

TEST(utf8_and_percent, "UTF-8 の検証とパーセントデコード") {
	CHECK(utf8_valid("こんにちは😀"));
	CHECK(!utf8_valid("\xC0\xAF"));			//冗長表現
	CHECK(!utf8_valid("\xED\xA0\x80"));		//サロゲート
	CHECK(!utf8_valid("\xF4\x90\x80\x80"));	//U+10FFFF 超
	CHECK(!utf8_valid("\xE3\x81"));			//途中で切れている

	std::string out;
	CHECK(percent_decode_strict("%E4%B8%96%E7%95%8C", out));
	CHECK_EQ(out, "世界");
	CHECK(!percent_decode_strict("%E0%A4%A", out));
	CHECK(!percent_decode_strict("%zz", out));
	CHECK(!percent_decode_strict("%FF", out));
	CHECK_EQ(percent_decode_form("a+b%20c%zz"), "a b c%zz");
	CHECK_EQ(percent_encode_component("a b/あ"), "a%20b%2F%E3%81%82");
}

TEST(base64_codec, "base64 / base64url の往復") {
	CHECK_EQ(base64_encode("hello"), "aGVsbG8=");
	CHECK_EQ(base64url_encode(std::string("\xfb\xff", 2)), "-_8");
	std::string out;
	CHECK(base64_decode("aGVsbG8=", out));
	CHECK_EQ(out, "hello");
	CHECK(base64_decode("-_8", out));
	CHECK_EQ(out, std::string("\xfb\xff", 2));
	CHECK(!base64_decode("a*b", out));
	CHECK_EQ(hex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(url_parse, "URL の分解") {
	Url u;
	CHECK(parse_url("https://Example.com/a/b?x=1#frag", u));
	CHECK_EQ(u.scheme, "https");
	CHECK_EQ(u.host, "example.com");
	CHECK_EQ(u.port, 443);
	CHECK_EQ(u.target, "/a/b?x=1");
	CHECK_EQ(u.origin(), "https://example.com");

	CHECK(parse_url("ws://[::1]:3000", u));
	CHECK_EQ(u.host, "::1");
	CHECK_EQ(u.port, 3000);
	CHECK_EQ(u.target, "/");
	CHECK_EQ(u.host_header(), "[::1]:3000");

	CHECK(parse_url("http://user:pw@host:8080/x", u));
	CHECK_EQ(u.userinfo, "user:pw");
	CHECK(!parse_url("not a url", u));
	CHECK(!parse_url("http://host:99999/", u));
	CHECK(!parse_url("http://ho st/", u));
}

TEST(event_loop_basics, "イベントループ: 他スレッドからの依頼とタイマーを処理する") {
	LoopThread lt;
	std::atomic<int> posted{0};
	std::atomic<int> ticks{0};
	std::atomic<bool> once{false};

	for (int i = 0; i < 100; ++i) lt.loop().post([&] { ++posted; });
	std::uint64_t repeating = 0;
	lt.loop().dispatch_sync([&] {
		repeating = lt.loop().set_timer(10, [&] { ++ticks; }, true);
		lt.loop().set_timer(20, [&] { once = true; });
	});

	CHECK(testing::wait_until([&] { return posted == 100 && ticks >= 3 && once; }));
	lt.loop().dispatch_sync([&] { lt.loop().cancel_timer(repeating); });
	const int after = ticks;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK_EQ(ticks.load(), after);
}

TEST(thread_pool_limit, "スレッドプール: 待ち行列の上限を超えた仕事は断る") {
	std::atomic<bool> release{false};
	std::atomic<int> done{0};
	int accepted = 0;
	{
		ThreadPool pool(1, 2);
		for (int i = 0; i < 10; ++i) {
			if (pool.submit([&] {
				while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				++done;
			})) ++accepted;
		}
		CHECK(accepted >= 2 && accepted <= 3);	//実行中 1 + 待ち 2
		release = true;
	}	//破棄時に受け付けた分は終わっている
	CHECK_EQ(done.load(), accepted);
}

TEST(event_loop_cancel_in_batch, "イベントループ: 同じ回に期限が来たタイマーでも、先のタスクが取り消したものは呼ばない") {
	LoopThread lt;
	std::atomic<int> fired{0};
	std::atomic<bool> first_ran{false};
	lt.loop().dispatch_sync([&] {
		std::uint64_t* second = new std::uint64_t(0);
		lt.loop().set_timer(1, [&, second] {
			first_ran = true;
			lt.loop().cancel_timer(*second);
			delete second;
		});
		*second = lt.loop().set_timer(2, [&] { ++fired; }, true);
		//両方の期限が過ぎてから timers を回させる (同じ回に集まる)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	});
	CHECK(testing::wait_until([&] { return first_ran.load(); }));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK_EQ(fired.load(), 0);	//呼ばれず、繰り返しとして積み直されもしない
}

TEST(event_loop_dispatch_during_stop, "イベントループ: 止まる途中に dispatch_sync しても取り残されず戻る") {
	for (int round = 0; round < 200; ++round) {
		EventLoop loop;
		std::atomic<bool> started{false};
		std::thread runner([&] {
			loop.post([&] { started = true; });
			loop.run();
		});
		while (!started) std::this_thread::yield();
		std::atomic<int> done{0};
		std::thread caller([&] {
			for (int i = 0; i < 50; ++i) loop.dispatch_sync([&] { ++done; });
		});
		loop.stop();
		runner.join();
		caller.join();	//止まった後の分はその場で実行される
		CHECK_EQ(done.load(), 50);
	}
}

TEST(thread_pool_self_destroy, "スレッドプール: 自分のワーカーの中から破棄しても落ちない") {
	auto pool = std::make_unique<ThreadPool>(2);
	std::atomic<bool> destroyed{false};
	std::atomic<int> others{0};
	for (int i = 0; i < 4; ++i) pool->submit([&] { ++others; });
	CHECK(testing::wait_until([&] { return others.load() == 4; }));
	std::atomic<bool> submitted{false};
	pool->submit([&] {
		while (!submitted) std::this_thread::yield();	//submit から戻ってから壊す (呼び出し中の破棄は呼び出し側の誤り)
		pool.reset();	//他のワーカーとは合流し、自分は切り離される
		destroyed = true;
	});
	submitted = true;
	CHECK(testing::wait_until([&] { return destroyed.load(); }));
}
