#include <filesystem>
#include <fstream>
#include <sstream>

#include "portus/id.hpp"
#include "portus/log.hpp"
#include "test.hpp"

using namespace portus;

TEST(log_redact, "ログ: 秘匿キーをマスクし、深さを制限する") {
	const Json obj = Json::object({
		{ "query", Json::object({ { "session", "s" }, { "token", "t" }, { "id", 1 } }) },
		{ "Authorization", "Bearer x" },
		{ "headers", Json::object({ { "Cookie", "sid=1" }, { "x-api-key", "k" }, { "accept", "*/*" } }) },
		{ "list", Json::array({ Json::object({ { "password", "p" } }) }) },
	});
	const Json out = redact(obj);
	CHECK_EQ(out.at("query").at("session").as_string(), "[REDACTED]");
	CHECK_EQ(out.at("query").at("token").as_string(), "[REDACTED]");
	CHECK_EQ(out.at("query").at("id").as_number(), 1);
	CHECK_EQ(out.at("Authorization").as_string(), "[REDACTED]");
	CHECK_EQ(out.at("headers").at("Cookie").as_string(), "[REDACTED]");
	CHECK_EQ(out.at("headers").at("x-api-key").as_string(), "[REDACTED]");
	CHECK_EQ(out.at("headers").at("accept").as_string(), "*/*");
	CHECK_EQ(out.at("list").items()[0].at("password").as_string(), "[REDACTED]");

	Json deep = Json("leaf");
	for (int i = 0; i < 10; ++i) deep = Json::object({ { "n", deep } });
	CHECK(redact(deep).dump().find("[Depth]") != std::string::npos);
}

TEST(log_format, "ログ: 1 レコード 1 行を保つ (改行で偽の行を作れない)") {
	LogRecord rec;
	rec.time = "2026-01-02T03:04:05.678Z";
	rec.level = LogLevel::Warn;
	rec.scope = "app.http";
	rec.msg = "hello\n2026-01-02T00:00:00.000Z ERROR [app] forged";
	rec.fields = Json::object({ { "path", "/a\r\nb" }, { "status", 200 }, { "ok", true } });

	const std::string pretty = format_pretty(rec);
	CHECK_EQ(pretty.find('\n'), std::string::npos);
	CHECK_EQ(pretty.find('\r'), std::string::npos);
	CHECK(pretty.find("2026-01-02T03:04:05.678Z WARN  [app.http] hello\\n") == 0);
	CHECK(pretty.find("path=/a\\r\\nb status=200 ok=true") != std::string::npos);

	const std::string line = format_json(rec);
	CHECK_EQ(line.find('\n'), std::string::npos);
	const Json parsed = Json::parse(line);
	CHECK_EQ(parsed.at("level").as_string(), "warn");
	CHECK_EQ(parsed.at("msg").as_string(), rec.msg);
	CHECK_EQ(parsed.at("status").as_number(), 200);
}

TEST(log_levels_child, "ログ: レベルで絞り、child がスコープと固定フィールドを足す") {
	auto sink = std::make_shared<testing::MemorySink>();
	Logger root("app", LogLevel::Info, { sink });
	Logger child = root.child("http", Json::object({ { "reqId", "r1" } }));

	root.debug("hidden");
	child.info("shown", Json::object({ { "token", "secret" }, { "n", 1 } }));
	root.set_level(LogLevel::Debug);	//レベルは親子で共有
	child.debug("now shown");

	const auto records = sink->records();
	CHECK_EQ(records.size(), 2u);
	CHECK_EQ(records[0].scope, "app.http");
	CHECK_EQ(records[0].fields.at("reqId").as_string(), "r1");
	CHECK_EQ(records[0].fields.at("token").as_string(), "[REDACTED]");
	CHECK_EQ(records[1].msg, "now shown");
}

TEST(log_file_async, "ログ: 専用スレッド経由で日別ファイルへ JSON Lines を書く") {
	//日本語を含むパスでも書ける (Windows で ANSI コードページに落とすと別の名前になるか開けない)
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::filesystem::u8path("portus-log-ログ-" + uuidv4());
	{
		auto file = std::make_shared<DailyFileSink>(dir.u8string(), "svc");	//DailyFileSink のパスは UTF-8
		auto async = std::make_shared<AsyncSink>(SinkList{ file });
		Logger log("svc", LogLevel::Info, { async });
		for (int i = 0; i < 500; ++i) log.info("line", Json::object({ { "i", i } }));
		log.flush();

		int lines = 0;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			CHECK(entry.path().filename().string().rfind("svc-", 0) == 0);
			std::ifstream in(entry.path());
			std::string line;
			while (std::getline(in, line)) {
				CHECK_EQ(Json::parse(line).at("i").as_number(), lines);
				++lines;
			}
		}
		CHECK_EQ(lines, 500);
	}
	std::filesystem::remove_all(dir);
}

TEST(log_unwritable_dir, "ログ: 書けないディレクトリでも落ちない") {
	//親が普通のファイルなら、どの OS でもディレクトリは作れずファイルも開けない
	//  (/proc のような OS 固有のパスは Windows では今のドライブ直下の普通のパスになり、作れてしまう)
	const std::filesystem::path blocker = std::filesystem::temp_directory_path() / ("portus-log-file-" + uuidv4());
	std::ofstream(blocker, std::ios::binary) << "x";
	const std::filesystem::path dir = blocker / "x";
	{
		DailyFileSink sink(dir.u8string(), "svc");
		LogRecord rec;
		rec.time = "2026-01-02T03:04:05.678Z";
		sink.write(rec);
		sink.flush();
	}
	std::error_code ec;
	CHECK(!std::filesystem::exists(dir, ec));
	CHECK(std::filesystem::is_regular_file(blocker));
	std::filesystem::remove(blocker);
}

TEST(log_async_drop, "ログ: 出力が詰まったら溜め込まずに捨て、件数を残す") {
	struct SlowSink : LogSink {
		std::atomic<int> written{0};
		std::atomic<int> dropped_notice{0};
		void write(const LogRecord& rec) override {
			if (rec.msg == "log records dropped") dropped_notice += static_cast<int>(rec.fields.at("count").as_number());
			else ++written;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};
	auto slow = std::make_shared<SlowSink>();
	AsyncSink async(SinkList{ slow }, 8);
	LogRecord rec;
	rec.msg = "x";
	for (int i = 0; i < 200; ++i) async.write(rec);
	async.flush();
	CHECK(slow->written < 200);
	CHECK_EQ(slow->written + slow->dropped_notice, 200);
}
