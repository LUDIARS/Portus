//最小のサーバ例: HTTP (8080) と WebSocket (3000) を 1 本のイベントループで動かす
//  cmake -S . -B build && cmake --build build && ./build/portus_hello
#include <csignal>

#include "portus/env.hpp"
#include "portus/http.hpp"
#include "portus/log.hpp"
#include "portus/ws.hpp"

using namespace portus;

static EventLoop* g_loop = nullptr;

int main() {
	load_env();
	Logger log = logger_from_env("app");
	install_terminate_handler(log);

	EventLoop loop;
	g_loop = &loop;
	std::signal(SIGINT, [](int) { g_loop->stop(); });
	std::signal(SIGTERM, [](int) { g_loop->stop(); });

	HttpServerOptions options;
	options.log = log.child("http");
	options.allowed_hosts = { "127.0.0.1", "localhost" };	//ブラウザ経由の DNS リバインディングを防ぐ

	//"モジュール#関数" で参照するハンドラ。表に載せた名前だけが呼べる
	options.handlers["user#byId"] = [](RequestContext& ctx) -> Reply {
		return Json::object({ { "id", ctx.params.at("id") } });
	};

	//表に書いたものだけが到達可能。上から順に評価し、最初に一致したものを使う
	options.routes = {
		{ "GET", "/user/{id:number}", "user#byId" },
		{ "GET", "/hello/{name}", [](RequestContext& ctx) -> Reply { return "hello " + ctx.params.at("name").as_string(); } },
		{ "POST", "/echo", [](RequestContext& ctx) -> Reply { return ctx.query; }, Json::object({ { "public", true } }) },
		{ "GET", "/health", [](RequestContext&) -> Reply { return "ok"; }, Json::object({ { "public", true } }) },
	};

	//認証などはミドルウェアで足す。next() を呼ばずに返せば打ち切り
	options.middlewares.push_back([](RequestContext& ctx, const Next& next) -> Reply {
		if (!ctx.route->meta.at("public").as_bool() && !ctx.headers.find("authorization")) return text("unauthorized", 401);
		return next();
	});

	HttpServer http(loop, std::move(options));
	http.listen(8080);

	WebSocketServerOptions ws_options;
	ws_options.allowed_origins = { "http://127.0.0.1:8080", "http://localhost:8080" };
	WebSocketServer wss(loop, ws_options);
	wss.on_connection = [](const std::shared_ptr<WebSocket>& ws, const RequestHead&) {
		std::weak_ptr<WebSocket> weak = ws;
		ws->on_message = [weak](std::string_view data, bool) {
			if (auto s = weak.lock()) s->send("echo:" + std::string(data));
		};
	};
	log.info("websocket server listening", Json::object({ { "port", wss.listen(3000) } }));

	loop.run();	//SIGINT / SIGTERM まで

	wss.close();
	http.close();
	log.flush();
	return 0;
}
