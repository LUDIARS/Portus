//最小のサーバ例: HTTP と WebSocket (/ws) を 8080 で受ける
//  cmake -S . -B build && cmake --build build && ./build/portus_hello
#include <csignal>

#include "portus/env.hpp"
#include "portus/http.hpp"
#include "portus/http_websocket.hpp"
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

	WebSocketUpgradeOptions ws_options;
	//ローカルの公開 echo。ブラウザからは同じ Origin だけ受け付ける。
	ws_options.authorize = [](const RequestHead& req, std::string_view) -> std::optional<Json> {
		const auto* origin = req.headers.find("origin");
		if (origin && *origin != "http://127.0.0.1:8080" && *origin != "http://localhost:8080") return std::nullopt;
		return Json{};
	};
	ws_options.on_connection = [](const std::shared_ptr<WebSocket>& ws, const RequestHead&, const Json&) {
		std::weak_ptr<WebSocket> weak = ws;
		ws->on_message = [weak](std::string_view data, bool) {
			if (auto socket = weak.lock()) socket->send("echo:" + std::string(data));
		};
	};
	options.websocket_upgrade = std::move(ws_options);
	HttpServer http(loop, std::move(options));
	http.listen(8080);

	loop.run();	//SIGINT / SIGTERM まで

	http.close();
	log.flush();
	return 0;
}
