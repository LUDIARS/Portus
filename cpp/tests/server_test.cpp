#include "portus/http.hpp"
#include "portus/net.hpp"
#include "test.hpp"

using namespace portus;

namespace {

struct Fixture {
	LoopThread lt;
	std::shared_ptr<testing::MemorySink> sink = std::make_shared<testing::MemorySink>();
	std::unique_ptr<HttpServer> server;
	int port = 0;

	explicit Fixture(HttpServerOptions options) {
		options.log = Logger("test", LogLevel::Debug, { sink });
		server.reset(new HttpServer(lt.loop(), std::move(options)));
		port = server->listen(0);
	}
	~Fixture() { server->close(); }

	std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

	FetchResponse get(const std::string& path, HeaderList headers = {}) const {
		FetchRequest req;
		req.headers = std::move(headers);
		req.timeout_ms = 5000;
		return fetch(url(path), req);
	}

	FetchResponse post(const std::string& path, const std::string& body) const {
		FetchRequest req;
		req.method = "POST";
		req.body = body;
		req.timeout_ms = 5000;
		return fetch(url(path), req);
	}
};

}

TEST(server_basics, "サーバ: 関数ハンドラ・ボディ結合・応答の型変換・アクセスログ") {
	HttpServerOptions options;
	options.max_body_bytes = 1024;
	options.routes = {
		{ "GET", "/hello/{name}", [](RequestContext& ctx) -> Reply { return "hello " + ctx.params.at("name").as_string(); } },
		{ "POST", "/echo", [](RequestContext& ctx) -> Reply { return Json::object({ { "got", ctx.query.at("a") }, { "session", ctx.query.at("session") } }); } },
		{ "GET", "/empty", [](RequestContext&) -> Reply { return Reply::no_content(); } },
		{ "GET", "/teapot", [](RequestContext&) -> Reply { return json(Json::object({ { "short", "stout" } }), 418); } },
		{ "GET", "/boom", [](RequestContext&) -> Reply { throw std::runtime_error("boom"); } },
	};
	Fixture f(std::move(options));

	const FetchResponse r1 = f.get("/hello/%E4%B8%96%E7%95%8C");
	CHECK_EQ(r1.status, 200);
	CHECK_EQ(r1.body, "hello 世界");
	CHECK_EQ(r1.headers.get("content-type"), "text/plain; charset=utf-8");
	CHECK_EQ(r1.headers.get("x-request-id").size(), 36u);

	const FetchResponse r2 = f.post("/echo", "{\"a\":1,\"session\":\"secret-session\"}");
	CHECK(Json::parse(r2.body) == Json::object({ { "got", 1 }, { "session", "secret-session" } }));
	CHECK_EQ(r2.headers.get("content-type"), "application/json; charset=utf-8");

	const FetchResponse r3 = f.post("/echo", "a=x&b=y");
	CHECK_EQ(Json::parse(r3.body).at("got").as_string(), "x");

	CHECK_EQ(f.get("/empty").status, 204);
	CHECK_EQ(f.get("/teapot").status, 418);
	CHECK_EQ(f.get("/nothing").status, 404);
	CHECK_EQ(f.post("/echo", std::string(4096, 'x')).status, 413);

	const FetchResponse r4 = f.get("/boom");
	CHECK_EQ(r4.status, 500);
	CHECK_EQ(r4.body, "internal error.");	//例外の中身は外へ出さない

	const auto records = f.sink->records();
	int access = 0;
	bool hello_logged = false;
	bool failure_logged = false;
	std::string all;
	for (const LogRecord& r : records) {
		all += format_json(r);
		if (r.msg == "access") {
			++access;
			if (r.fields.at("path").as_string() == "/hello/%E4%B8%96%E7%95%8C" && r.fields.at("status").as_number() == 200) hello_logged = true;
		}
		if (r.level == LogLevel::Error && r.msg == "handler failed" && r.fields.at("err").as_string() == "boom") failure_logged = true;
	}
	CHECK_EQ(access, 8);
	CHECK(hello_logged);
	CHECK(failure_logged);
	CHECK_EQ(all.find("secret-session"), std::string::npos);	//ログにボディ・クエリは出さない
}

TEST(server_middleware, "サーバ: ミドルウェアで打ち切り・前後処理ができる") {
	HttpServerOptions options;
	options.middlewares.push_back([](RequestContext& ctx, const Next& next) -> Reply {
		if (ctx.route->meta.at("auth").as_bool() && ctx.headers.get("x-token") != "ok") return text("unauthorized", 401);
		ctx.state.set("user", "alice");
		Reply reply = next();
		reply.headers.emplace_back("x-wrapped", "1");
		return reply;
	});
	options.routes = {
		{ "GET", "/me", [](RequestContext& ctx) -> Reply { return Json::object({ { "user", ctx.state.at("user") } }); }, Json::object({ { "auth", true } }) },
	};
	Fixture f(std::move(options));

	CHECK_EQ(f.get("/me").status, 401);
	const FetchResponse ok = f.get("/me", { { "X-Token", "ok" } });
	CHECK_EQ(ok.status, 200);
	CHECK_EQ(Json::parse(ok.body).at("user").as_string(), "alice");
	CHECK_EQ(ok.headers.get("x-wrapped"), "1");
}

TEST(server_handler_table, "サーバ: 表に載せたハンドラ名だけが呼べ、ルート表は止めずに差し替えられる") {
	HttpServerOptions options;
	options.handlers["greet#hello"] = [](RequestContext&) -> Reply { return "v1"; };
	options.handlers["greet#bye"] = [](RequestContext&) -> Reply { return "bye"; };
	options.routes = {
		{ "GET", "/hello", "greet#hello" },
		{ "GET", "/ghost", "greet#missing" },
	};
	Fixture f(std::move(options));

	CHECK_EQ(f.get("/hello").body, "v1");
	CHECK_EQ(f.get("/ghost").status, 404);
	CHECK_EQ(f.get("/bye").status, 404);

	f.server->set_routes({ { "GET", "/hello", "greet#hello" }, { "GET", "/bye", "greet#bye" } });
	CHECK_EQ(f.get("/bye").body, "bye");

	//壊れた表は採用しない
	CHECK_THROWS(f.server->set_routes({ { "GET", "/x", "../evil#run" } }));
	CHECK_EQ(f.get("/bye").body, "bye");
	CHECK_EQ(f.server->routes().size(), 2u);
}

TEST(server_keep_alive, "サーバ: 1 接続で続けて届いた要求を順に処理する") {
	HttpServerOptions options;
	options.routes = {
		{ "GET", "/a", [](RequestContext&) -> Reply { return "first"; } },
		{ "POST", "/b", [](RequestContext& ctx) -> Reply { return "second:" + ctx.body; } },
	};
	Fixture f(std::move(options));

	const std::string raw = testing::raw_exchange(f.port,
		"GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
		"POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi");
	const std::size_t first = raw.find("first");
	const std::size_t second = raw.find("second:hi");
	CHECK(first != std::string::npos && second != std::string::npos && first < second);
	CHECK(raw.find("connection: keep-alive") < first);
	CHECK(raw.find("connection: close") > first);
}

TEST(server_rejects, "サーバ: 危険な要求を拒否する (Host / chunked / 長さの食い違い / 巨大ヘッダ / 応答分割)") {
	HttpServerOptions options;
	options.allowed_hosts = { "127.0.0.1", "localhost" };
	options.max_header_bytes = 512;
	options.routes = {
		{ "GET", "/ok", [](RequestContext&) -> Reply { return "ok"; } },
		{ "POST", "/user/{id:number}", [](RequestContext& ctx) -> Reply { return Json::object({ { "id", ctx.query.at("id") } }); } },
		{ "GET", "/split", [](RequestContext& ctx) -> Reply { return text("x", 200, { { "x-echo", ctx.query.at("v").as_string() } }); } },
	};
	Fixture f(std::move(options));
	const auto status_of = [&](const std::string& request) { return testing::raw_exchange(f.port, request).substr(0, 12); };

	CHECK_EQ(status_of("GET /ok HTTP/1.1\r\nHost: localhost:8080\r\nConnection: close\r\n\r\n"), "HTTP/1.1 200");
	//DNS リバインディング: ブラウザが攻撃者のホスト名のままローカルサービスへ届く
	CHECK_EQ(status_of("GET /ok HTTP/1.1\r\nHost: attacker.example\r\n\r\n"), "HTTP/1.1 421");
	CHECK_EQ(status_of("GET /ok HTTP/1.1\r\n\r\n"), "HTTP/1.1 421");
	CHECK_EQ(status_of("POST /user/1 HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"), "HTTP/1.1 411");
	CHECK_EQ(status_of("POST /user/1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"), "HTTP/1.1 400");
	CHECK_EQ(status_of("GET /ok HTTP/1.1\r\nHost: localhost\r\nX-Big: " + std::string(1000, 'a') + "\r\n\r\n"), "HTTP/1.1 431");
	CHECK_EQ(status_of("GARBAGE\r\n\r\n"), "HTTP/1.1 400");

	//パス変数はボディで上書きできない
	const std::string body = "{\"id\":999}";
	const std::string raw = testing::raw_exchange(f.port,
		"POST /user/7 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
	CHECK(raw.find("{\"id\":7}") != std::string::npos);

	//ハンドラが外部入力をヘッダへ入れても、改行で応答を分割させない
	const std::string split = testing::raw_exchange(f.port, "GET /split?v=a%0d%0aSet-Cookie:%20x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	CHECK_EQ(split.substr(0, 12), "HTTP/1.1 500");
	CHECK_EQ(split.find("Set-Cookie"), std::string::npos);
}

TEST(server_slow_client, "サーバ: 要求を送り切らない接続は時間で切る") {
	HttpServerOptions options;
	options.request_timeout_ms = 200;
	options.routes = { { "GET", "/ok", [](RequestContext&) -> Reply { return "ok"; } } };
	Fixture f(std::move(options));

	const auto started = std::chrono::steady_clock::now();
	const std::string raw = testing::raw_exchange(f.port, "GET /ok HTTP/1.1\r\nHost: x\r\nX-Slow: ", 5000);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK_EQ(raw, "");
	CHECK(ms < 2000);
}

TEST(server_responsive, "サーバ: 遅いハンドラの実行中も他の要求にすぐ応える") {
	HttpServerOptions options;
	options.worker_threads = 4;
	options.routes = {
		{ "GET", "/slow", [](RequestContext&) -> Reply {
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
			return "slow";
		} },
		{ "GET", "/fast", [](RequestContext&) -> Reply { return "fast"; } },
	};
	Fixture f(std::move(options));

	std::vector<std::thread> slow;
	std::atomic<int> slow_done{0};
	for (int i = 0; i < 3; ++i) {
		slow.emplace_back([&] {
			if (f.get("/slow").body == "slow") ++slow_done;
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	const auto started = std::chrono::steady_clock::now();
	CHECK_EQ(f.get("/fast").body, "fast");
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(ms < 300);

	for (std::thread& t : slow) t.join();
	CHECK_EQ(slow_done.load(), 3);
}

TEST(server_busy, "サーバ: ワーカー待ちが上限を超えたら溜め込まずに 503 を返す") {
	std::atomic<bool> release{false};
	HttpServerOptions options;
	options.worker_threads = 1;
	options.max_pending = 1;
	options.routes = {
		{ "GET", "/wait", [&release](RequestContext&) -> Reply {
			while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			return "done";
		} },
	};
	Fixture f(std::move(options));

	std::atomic<int> ok{0};
	std::atomic<int> busy{0};
	std::vector<std::thread> clients;
	for (int i = 0; i < 6; ++i) {
		clients.emplace_back([&] {
			const int status = f.get("/wait").status;
			if (status == 200) ++ok;
			if (status == 503) ++busy;
		});
	}
	CHECK(testing::wait_until([&] { return busy >= 4; }));
	release = true;
	for (std::thread& t : clients) t.join();
	CHECK_EQ(ok + busy, 6);
	CHECK(ok >= 1 && ok <= 2);
}

TEST(server_inline, "サーバ: ワーカー無し (ループスレッドで直接実行) でも同じ結果になり、大量の連続要求でスタックを使い切らない") {
	HttpServerOptions options;
	options.worker_threads = 0;
	options.routes = {
		{ "GET", "/n/{i:number}", [](RequestContext& ctx) -> Reply { return Json::object({ { "i", ctx.params.at("i") } }); } },
		{ "GET", "/boom", [](RequestContext&) -> Reply { throw std::runtime_error("boom"); } },
	};
	Fixture f(std::move(options));

	CHECK_EQ(f.get("/n/5").body, "{\"i\":5}");
	CHECK_EQ(f.get("/boom").status, 500);
	CHECK_EQ(f.get("/none").status, 404);

	//1 回の送信に 3000 件を詰める
	std::string batch;
	for (int i = 0; i < 2999; ++i) batch += "GET /n/" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\n\r\n";
	batch += "GET /n/2999 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
	const std::string raw = testing::raw_exchange(f.port, batch, 10000);
	std::size_t count = 0;
	for (std::size_t pos = raw.find("HTTP/1.1 200"); pos != std::string::npos; pos = raw.find("HTTP/1.1 200", pos + 1)) ++count;
	CHECK_EQ(count, 3000u);
	CHECK(raw.find("{\"i\":2999}") != std::string::npos);
}

namespace {

void send_all(socket_t fd, const std::string& data) {
	std::size_t sent = 0;
	while (sent < data.size()) {
		const long n = socket_send(fd, data.data() + sent, data.size() - sent);
		if (n > 0) sent += static_cast<std::size_t>(n);
		else if (!last_error_would_block()) return;
		else wait_socket(fd, kWrite, 100);
	}
}

//読めた量を返す。eof には相手が閉じたか (時間切れでなく) を入れる。pause_ms ごとに休んで遅い読み手を真似る
std::string read_until(socket_t fd, std::size_t limit, int timeout_ms, bool* eof = nullptr, int pause_ms = 0) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	std::string out;
	std::vector<char> buf(65536);
	if (eof) *eof = false;
	while (out.size() < limit && std::chrono::steady_clock::now() < deadline) {
		const long n = socket_recv(fd, buf.data(), buf.size());
		if (n > 0) {
			out.append(buf.data(), static_cast<std::size_t>(n));
			if (pause_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
			continue;
		}
		if (n == 0 || !last_error_would_block()) {
			if (eof) *eof = true;
			break;
		}
		wait_socket(fd, kRead, 20);
	}
	return out;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
	std::size_t count = 0;
	for (std::size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
	return count;
}

}

TEST(server_pipeline_backpressure, "サーバ: 応答を読まずに要求だけ送り続ける相手には、送り切るまで次の要求を処理しない") {
	const std::string big(512 * 1024, 'x');
	std::atomic<int> calls{0};
	HttpServerOptions options;
	options.routes = { { "GET", "/big", [&](RequestContext&) -> Reply {
		++calls;
		return big;
	} } };
	Fixture f(std::move(options));

	const int total = 40;
	std::string batch;
	for (int i = 0; i < total; ++i) batch += "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
	const socket_t fd = connect_tcp("127.0.0.1", f.port, 2000);
	testing::set_receive_buffer(fd, 64 * 1024);
	send_all(fd, batch);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	//OS の送受信バッファに入る分 (数 MB) だけ先に進む。全件の応答を溜め込まない
	CHECK(calls.load() < 20);

	//読み始めれば残りも順に届く
	const std::size_t expected = static_cast<std::size_t>(total) * big.size();
	const std::string raw = read_until(fd, expected + static_cast<std::size_t>(total) * 200, 5000);
	close_socket(fd);
	CHECK_EQ(calls.load(), total);
	CHECK_EQ(count_of(raw, "HTTP/1.1 200"), static_cast<std::size_t>(total));
}

TEST(server_slow_reader, "サーバ: 遅くても受け取っている相手には keep-alive の期限で応答を切らず、送り切ってから次の要求を待つ") {
	const std::string big(8 * 1024 * 1024, 'x');
	HttpServerOptions options;
	options.keep_alive_timeout_ms = 300;
	options.routes = {
		{ "GET", "/big", [&](RequestContext&) -> Reply { return big; } },
		{ "GET", "/small", [](RequestContext&) -> Reply { return "small"; } },
	};
	Fixture f(std::move(options));

	const socket_t fd = connect_tcp("127.0.0.1", f.port, 2000);
	testing::set_receive_buffer(fd, 64 * 1024);
	send_all(fd, "GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
	//keep-alive の期限より長くかけて先頭を読む (この間サーバはまだ送り切っていない)
	const auto started = std::chrono::steady_clock::now();
	bool eof = false;
	const std::string slow = read_until(fd, 2 * 1024 * 1024, 5000, &eof, 10);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(!eof);
	CHECK(ms > 300);
	const std::string fast = read_until(fd, big.size() - slow.size(), 5000, &eof);
	CHECK(!eof);
	CHECK(slow.size() + fast.size() >= big.size());

	//送り切った後で keep-alive の待ちが始まるので、同じ接続で次の要求が通る
	send_all(fd, "GET /small HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
	const std::string rest = read_until(fd, 1 << 20, 2000, &eof);
	close_socket(fd);
	CHECK(rest.find("small") != std::string::npos);
	CHECK(eof);
}

TEST(server_write_stall, "サーバ: 応答をまったく受け取らない相手は write_timeout_ms で切る") {
	const std::string huge(8 * 1024 * 1024, 'x');
	HttpServerOptions options;
	options.write_timeout_ms = 150;
	options.routes = { { "GET", "/huge", [&](RequestContext&) -> Reply { return huge; } } };
	Fixture f(std::move(options));

	const socket_t fd = connect_tcp("127.0.0.1", f.port, 2000);
	testing::set_receive_buffer(fd, 64 * 1024);
	send_all(fd, "GET /huge HTTP/1.1\r\nHost: x\r\n\r\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	bool eof = false;
	const std::string raw = read_until(fd, huge.size() * 2, 2000, &eof);
	close_socket(fd);
	CHECK(eof);
	CHECK(raw.size() < huge.size());	//OS のバッファにあった分だけ届いて切れる
}

TEST(server_close_in_handler, "サーバ: ハンドラの中から close してもプロセスは落ちず、待ち受けをやめる") {
	std::atomic<HttpServer*> server{nullptr};
	std::atomic<bool> closed{false};
	HttpServerOptions options;
	options.worker_threads = 2;
	options.routes = { { "GET", "/quit", [&](RequestContext&) -> Reply {
		server.load()->close();
		closed = true;
		return "bye";
	} } };
	Fixture f(std::move(options));
	server = f.server.get();

	testing::raw_exchange(f.port, "GET /quit HTTP/1.1\r\nHost: x\r\n\r\n", 2000);
	CHECK(testing::wait_until([&] { return closed.load(); }));
	CHECK_THROWS(connect_tcp("127.0.0.1", f.port, 500));
}

#ifndef _WIN32
TEST(server_accept_backoff, "サーバ: fd を使い切って accept が失敗しても空回りせず、空いたら受け付けを再開する") {
	HttpServerOptions options;
	options.routes = { { "GET", "/ok", [](RequestContext&) -> Reply { return "ok"; } } };
	Fixture f(std::move(options));

	socket_t fd = kInvalidSocket;
	double cpu = 0;
	{
		testing::FdExhaustion full(1);
		CHECK(full.ok());
		fd = connect_tcp("127.0.0.1", f.port, 2000);	//最後の 1 本を使う。サーバ側の accept は EMFILE になる
		cpu = testing::cpu_seconds_during(300);
	}
	CHECK(cpu < 0.1);

	send_all(fd, "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
	const std::string raw = read_until(fd, 1 << 20, 3000);
	close_socket(fd);
	CHECK_EQ(raw.substr(0, 12), "HTTP/1.1 200");
}
#endif
