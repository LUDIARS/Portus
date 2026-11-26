#include "portus/http.hpp"
#include "portus/http_websocket.hpp"
#include "test.hpp"

using namespace portus;

namespace {

constexpr const char* kUpgradeHeaders =
	"Host: localhost\r\n"
	"Upgrade: h2c\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
	"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
constexpr const char* kHttpRequest = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
constexpr const char* kApprovedHeaders = "Origin: https://actio.example\r\nAuthorization: Bearer fixture\r\n";

std::string upgrade_request(std::string_view target = "/ws", std::string_view extra = kApprovedHeaders) {
	return "GET " + std::string(target) + " HTTP/1.1\r\n" + kUpgradeHeaders + std::string(extra) + "\r\n";
}

struct Observations {
	std::weak_ptr<WebSocket> socket;
	std::atomic<unsigned> authorized{0};
	std::atomic<unsigned> opened{0};
	std::atomic<unsigned> closed{0};
};

HttpServerOptions shared_options(EventLoop& loop, Observations& observed, std::size_t max_connections = 1024) {
	HttpServerOptions options;
	options.routes = {
		{ "GET", "/health", [](RequestContext&) -> Reply { return "healthy"; } },
		{ "GET", "/ws", [](RequestContext&) -> Reply { return "ordinary HTTP"; } },
	};
	options.allowed_hosts = { "localhost" };
	options.max_connections = max_connections;
	WebSocketUpgradeOptions ws;
	ws.max_payload = 32;
	ws.authorize = [&](const RequestHead& request, std::string_view peer) -> std::optional<Json> {
		CHECK(loop.in_loop_thread());
		CHECK(!peer.empty());
		++observed.authorized;
		if (request.headers.get("origin") != "https://actio.example") return std::nullopt;
		if (request.headers.get("authorization") != "Bearer fixture") return std::nullopt;
		return Json::object({ { "userId", "alice" } });
	};
	ws.on_connection = [&](const std::shared_ptr<WebSocket>& socket, const RequestHead&, const Json& authorization) {
		CHECK(loop.in_loop_thread());
		observed.socket = socket;
		++observed.opened;
		socket->on_message = [socket, user = authorization.at("userId").as_string()](std::string_view data, bool) {
			socket->send(user + ":" + std::string(data));
		};
		socket->on_close = [&](int, std::string_view) { ++observed.closed; };
	};
	options.websocket_upgrade = std::move(ws);
	return options;
}

struct Fixture {
	Observations observed;
	LoopThread thread;
	HttpServer server;
	int port;

	explicit Fixture(std::size_t max_connections = 1024)
		: server(thread.loop(), shared_options(thread.loop(), observed, max_connections)), port(server.listen(0)) {}

	bool expired() {
		bool result = false;
		thread.loop().dispatch_sync([&] { result = observed.socket.expired(); });
		return result;
	}
};

}

TEST(http_websocket_routes, "共有 listener: HTTP 応答の後で認証済み WebSocket に切り替え、先読みを引き継ぐ") {
	Fixture f;
	const std::string close_code("\x03\xE8", 2);
	const auto response = testing::raw_exchange(f.port,
		"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n" + upgrade_request("/ws?client=actio")
		+ encode_frame(Opcode::Text, "hello", true) + encode_frame(Opcode::Close, close_code, true));
	CHECK_EQ(response.substr(0, 12), "HTTP/1.1 200");
	const auto handshake = response.find("healthyHTTP/1.1 101 Switching Protocols\r\n");
	CHECK(handshake != std::string::npos);
	if (handshake == std::string::npos) return;
	const auto frame_start = response.find("\r\n\r\n", handshake);
	CHECK(frame_start != std::string::npos);
	if (frame_start == std::string::npos) return;
	CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n", handshake) < frame_start + 4);
	const auto frames = FrameParser(false, 32).push(std::string_view(response).substr(frame_start + 4));
	CHECK_EQ(frames.size(), 2u);
	if (frames.size() == 2) {
		CHECK(frames[0].opcode == Opcode::Text && frames[0].payload == "alice:hello");
		CHECK(frames[1].opcode == Opcode::Close && frames[1].payload == close_code);
	}
	CHECK(testing::wait_until([&] { return f.expired(); }));
	CHECK_EQ(f.observed.closed.load(), 1u);
	CHECK_EQ(f.observed.authorized.load(), 1u);
	CHECK_EQ(f.observed.opened.load(), 1u);
	CHECK(testing::raw_exchange(f.port, kHttpRequest).find("healthy") != std::string::npos);
}

TEST(http_websocket_refusals, "共有 listener: パスと HTTP の制限を守り、認可前の不正な要求を拒む") {
	Fixture f;
	const auto status = [&](const std::string& request) { return testing::raw_exchange(f.port, request).substr(0, 12); };
	CHECK_EQ(status("GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"), "HTTP/1.1 200");
	CHECK_EQ(status(std::string("GET /other HTTP/1.1\r\nConnection: close\r\n") + kUpgradeHeaders + "\r\n"), "HTTP/1.1 404");
	CHECK_EQ(status(upgrade_request("/ws", "Content-Length: 1\r\nExpect: 100-continue\r\n")), "HTTP/1.1 400");
	CHECK_EQ(status(upgrade_request("/ws", "Transfer-Encoding: chunked\r\n")), "HTTP/1.1 411");
	CHECK_EQ(status(upgrade_request("/ws", "Sec-WebSocket-Version: 13\r\n")), "HTTP/1.1 400");
	CHECK_EQ(status(upgrade_request("/ws", "Origin: https://actio.example\r\nOrigin: https://other.example\r\n")), "HTTP/1.1 400");
	CHECK_EQ(status(upgrade_request("/ws", "X-Large: " + std::string(17 * 1024, 'x') + "\r\n")), "HTTP/1.1 431");
	CHECK_EQ(f.observed.authorized.load(), 0u);
	CHECK_EQ(status(upgrade_request("/ws", "Origin: https://other.example\r\nAuthorization: Bearer fixture\r\n")), "HTTP/1.1 403");
	CHECK_EQ(status(upgrade_request("/ws", "Origin: https://actio.example\r\n")), "HTTP/1.1 403");
	CHECK_EQ(f.observed.authorized.load(), 2u);
	CHECK_EQ(f.observed.opened.load(), 0u);
}

TEST(http_websocket_limits, "共有 listener: 接続数とペイロードの上限を守り、サーバの close で循環を切る") {
	Fixture f(1);
	const std::string limit_code("\x03\xF1", 2);
	const std::string oversized = testing::raw_exchange(f.port,
		upgrade_request() + encode_frame(Opcode::Text, std::string(33, 'x'), true));
	CHECK_EQ(oversized.substr(0, 12), "HTTP/1.1 101");
	CHECK(oversized.find(encode_frame(Opcode::Close, limit_code, false)) != std::string::npos);
	CHECK(testing::wait_until([&] { return f.expired(); }));

	const auto fd = connect_tcp("127.0.0.1", f.port, 3000);
	const std::string request = upgrade_request();
	const auto sent = socket_send(fd, request.data(), request.size());
	const bool opened = testing::wait_until([&] { return f.observed.opened.load() == 2; });
	const std::string refused = opened ? testing::raw_exchange(f.port, kHttpRequest) : std::string{};
	f.server.close();
	close_socket(fd);
	CHECK_EQ(sent, static_cast<long>(request.size()));
	CHECK(opened);
	CHECK(refused.empty());
	CHECK(f.expired());
	CHECK_EQ(f.observed.closed.load(), 2u);
}

TEST(http_websocket_configuration, "共有 listener: パス・認可・コールバック・上限を構築時に検査する") {
	Observations observed;
	EventLoop loop;
	const auto initial = shared_options(loop, observed);
	auto invalid = initial;
	invalid.websocket_upgrade->path = "/ws?query";
	CHECK_THROWS(HttpServer(loop, invalid));
	invalid = initial;
	invalid.websocket_upgrade->authorize = nullptr;
	CHECK_THROWS(HttpServer(loop, invalid));
	invalid = initial;
	invalid.websocket_upgrade->on_connection = nullptr;
	CHECK_THROWS(HttpServer(loop, invalid));
	invalid = initial;
	invalid.websocket_upgrade->max_buffered = 0;
	CHECK_THROWS(HttpServer(loop, invalid));
	invalid = initial;
	invalid.websocket_upgrade->max_payload = 0;
	CHECK_THROWS(HttpServer(loop, invalid));
}
