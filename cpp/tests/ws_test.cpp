#include <future>

#include "portus/ws.hpp"
#include "test.hpp"

using namespace portus;

namespace {

template <class T>
bool ready(std::future<T>& f, int ms = 3000) {
	return f.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
}

const char* const kUpgrade = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";

//ハンドシェイクの後に frames を送り、返ってきたフレームを解析する
std::vector<Frame> raw_session(int port, const std::string& frames, int timeout_ms = 1500) {
	const std::string raw = testing::raw_exchange(port, std::string(kUpgrade) + "\r\n" + frames, timeout_ms);
	const std::size_t body = raw.find("\r\n\r\n");
	if (body == std::string::npos) return {};
	return FrameParser(false, 1024 * 1024).push(std::string_view(raw).substr(body + 4));
}

struct EchoServer {
	LoopThread lt;
	WebSocketServer server;
	std::promise<int> closed_code;	//最初に閉じた接続のコード
	std::atomic<bool> closed_seen{false};
	int port = 0;

	explicit EchoServer(WebSocketServerOptions options = {}) : server(lt.loop(), options) {
		server.on_connection = [this](const std::shared_ptr<WebSocket>& ws, const RequestHead&) {
			std::weak_ptr<WebSocket> weak = ws;
			ws->on_message = [weak](std::string_view data, bool) {
				if (auto s = weak.lock()) s->send("echo:" + std::string(data));
			};
			ws->on_close = [this](int code, std::string_view) {
				if (!closed_seen.exchange(true)) closed_code.set_value(code);
			};
		};
		port = server.listen(0);
	}
	~EchoServer() { server.close(); }
	std::string url() const { return "ws://127.0.0.1:" + std::to_string(port) + "/"; }
};

}

TEST(ws_accept_key, "WebSocket: acceptKey は RFC 6455 の例と一致する") {
	CHECK_EQ(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(ws_validate_upgrade, "WebSocket: validateUpgrade は不正な要求を拒否する") {
	const auto make = [](const char* version, const char* key) {
		Headers h;
		h.add("Upgrade", "websocket");
		h.add("Connection", "keep-alive, Upgrade");
		h.add("Sec-WebSocket-Version", version);
		h.add("Sec-WebSocket-Key", key);
		return h;
	};
	CHECK(!validate_upgrade("GET", make("13", "dGhlIHNhbXBsZSBub25jZQ==")).has_value());
	CHECK(validate_upgrade("POST", make("13", "dGhlIHNhbXBsZSBub25jZQ==")).has_value());
	CHECK(validate_upgrade("GET", make("8", "dGhlIHNhbXBsZSBub25jZQ==")).has_value());
	CHECK(validate_upgrade("GET", make("13", "short")).has_value());
}

TEST(ws_frame_roundtrip, "WebSocket: フレームの往復 (7bit / 16bit / 64bit 長、マスク付き)") {
	for (const std::size_t size : { 0u, 5u, 125u, 126u, 70000u }) {
		const std::string payload(size, 'a');
		FrameParser parser(true, 1024 * 1024);
		const auto frames = parser.push(encode_frame(Opcode::Text, payload, true));
		CHECK_EQ(frames.size(), 1u);
		CHECK(frames[0].payload == payload);
	}
}

TEST(ws_frame_split, "WebSocket: 分割して届いたフレームも組み立てられる") {
	FrameParser parser(false, 1024);
	const std::string bytes = encode_frame(Opcode::Text, "hello", false) + encode_frame(Opcode::Binary, "x", false);
	std::vector<Frame> frames;
	for (const char c : bytes) {
		for (Frame& f : parser.push(std::string_view(&c, 1))) frames.push_back(std::move(f));
	}
	CHECK_EQ(frames.size(), 2u);
	CHECK_EQ(frames[0].payload, "hello");
	CHECK(frames[1].opcode == Opcode::Binary);
}

TEST(ws_frame_reject, "WebSocket: マスク無しフレーム・上限超過・RSV・不明なオペコードを拒否する") {
	const auto close_code = [](FrameParser parser, const std::string& bytes) {
		try {
			parser.push(bytes);
		} catch (const ProtocolError& ex) {
			return ex.close_code;
		}
		return 0;
	};
	CHECK_EQ(close_code(FrameParser(true, 1024), encode_frame(Opcode::Text, "x", false)), 1002);
	CHECK_EQ(close_code(FrameParser(true, 10), encode_frame(Opcode::Text, std::string(11, 'x'), true)), 1009);

	std::string rsv = encode_frame(Opcode::Text, "x", true);
	rsv[0] = static_cast<char>(rsv[0] | 0x40);
	CHECK_EQ(close_code(FrameParser(true, 1024), rsv), 1002);

	std::string unknown = encode_frame(Opcode::Text, "x", true);
	unknown[0] = static_cast<char>(0x83);
	CHECK_EQ(close_code(FrameParser(true, 1024), unknown), 1002);

	//分割された制御フレームは不正
	CHECK_EQ(close_code(FrameParser(true, 1024), encode_frame(Opcode::Ping, "x", true, false)), 1002);

	//64bit 長ヘッダだけで上限超過を検出する (本体を待たない)
	std::string header(14, '\0');
	header[0] = static_cast<char>(0x81);
	header[1] = static_cast<char>(0x80 | 127);
	header[4] = 0x01;	//2^40
	CHECK_EQ(close_code(FrameParser(true, 1024), header), 1009);
}

TEST(ws_echo, "WebSocket: サーバとクライアントでテキストの echo と ping/pong、正常終了ができる") {
	EchoServer s;
	std::promise<void> opened;
	std::promise<std::string> reply;
	std::promise<void> pong;
	std::promise<int> closed;
	std::shared_ptr<WebSocket> client;

	s.lt.loop().dispatch_sync([&] {
		client = connect_websocket(s.lt.loop(), s.url());
		client->on_open = [&] { opened.set_value(); };
		client->on_message = [&](std::string_view data, bool) { reply.set_value(std::string(data)); };
		client->on_pong = [&](std::string_view) { pong.set_value(); };
		client->on_close = [&](int code, std::string_view) { closed.set_value(code); };
	});

	auto opened_f = opened.get_future();
	CHECK(ready(opened_f));
	CHECK(client->ready_state() == WebSocket::OPEN);

	client->send("こんにちは");	//別スレッドから送れる
	auto reply_f = reply.get_future();
	CHECK(ready(reply_f));
	CHECK_EQ(reply_f.get(), "echo:こんにちは");

	client->ping();
	auto pong_f = pong.get_future();
	CHECK(ready(pong_f));

	client->close(1000, "bye");
	auto closed_f = closed.get_future();
	CHECK(ready(closed_f));
	CHECK_EQ(closed_f.get(), 1000);
	CHECK(client->ready_state() == WebSocket::CLOSED);

	auto server_closed = s.closed_code.get_future();
	CHECK(ready(server_closed));
	CHECK_EQ(server_closed.get(), 1000);
}

TEST(ws_too_large, "WebSocket: 上限を超えるメッセージは 1009 で切断される") {
	WebSocketServerOptions options;
	options.max_payload = 16;
	EchoServer s(options);

	std::shared_ptr<WebSocket> client;
	std::promise<int> client_closed;
	s.lt.loop().dispatch_sync([&] {
		client = connect_websocket(s.lt.loop(), s.url());
		std::weak_ptr<WebSocket> weak = client;
		client->on_open = [weak] {
			if (auto c = weak.lock()) c->send(std::string(100, 'x'));
		};
		client->on_close = [&](int code, std::string_view) { client_closed.set_value(code); };
	});

	auto server_closed = s.closed_code.get_future();
	CHECK(ready(server_closed));
	CHECK_EQ(server_closed.get(), 1009);
	auto client_f = client_closed.get_future();
	CHECK(ready(client_f));
	CHECK_EQ(client_f.get(), 1009);
}

TEST(ws_http_refusals, "WebSocket: WebSocket 以外の要求は 426、壊れたハンドシェイクは 400、許可外の Origin は 403") {
	WebSocketServerOptions options;
	options.allowed_origins = { "http://localhost:8080" };
	EchoServer s(options);

	CHECK_EQ(testing::raw_exchange(s.port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n").substr(0, 12), "HTTP/1.1 426");
	CHECK_EQ(testing::raw_exchange(s.port, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n").substr(0, 12), "HTTP/1.1 400");

	//ブラウザ上の別サイトからの接続 (Cross-Site WebSocket Hijacking) を止める
	CHECK_EQ(testing::raw_exchange(s.port, std::string(kUpgrade) + "Origin: https://evil.example\r\n\r\n").substr(0, 12), "HTTP/1.1 403");
	CHECK_EQ(testing::raw_exchange(s.port, std::string(kUpgrade) + "Origin: http://localhost:8080\r\n\r\n", 300).substr(0, 12), "HTTP/1.1 101");
	//Origin を付けないクライアント (ブラウザ以外) は通す
	CHECK_EQ(testing::raw_exchange(s.port, std::string(kUpgrade) + "\r\n", 300).substr(0, 12), "HTTP/1.1 101");
}

TEST(ws_fragments, "WebSocket: 分割メッセージを組み立て、ハンドシェイクと同時に届いたフレームも落とさない") {
	EchoServer s;
	const std::string frames =
		encode_frame(Opcode::Text, "he", true, false)
		+ encode_frame(Opcode::Ping, "p", true)	//分割の途中に制御フレームが挟まってよい
		+ encode_frame(Opcode::Continuation, "", true, false)
		+ encode_frame(Opcode::Continuation, "llo", true, true)
		+ encode_frame(Opcode::Close, std::string("\x03\xE8", 2), true);
	const auto replies = raw_session(s.port, frames);
	CHECK_EQ(replies.size(), 3u);
	if (replies.size() == 3) {
		CHECK(replies[0].opcode == Opcode::Pong && replies[0].payload == "p");
		CHECK(replies[1].opcode == Opcode::Text && replies[1].payload == "echo:hello");
		CHECK(replies[2].opcode == Opcode::Close && replies[2].payload == std::string("\x03\xE8", 2));
	}
}

TEST(ws_protocol_errors, "WebSocket: 不正な UTF-8・close フレーム・継続フレームは規定のコードで切断する") {
	const auto close_code_for = [](const std::string& frames) {
		EchoServer s;
		const auto replies = raw_session(s.port, frames);
		if (replies.empty() || replies.back().opcode != Opcode::Close || replies.back().payload.size() < 2) return 0;
		const std::string& p = replies.back().payload;
		return (static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1]);
	};
	CHECK_EQ(close_code_for(encode_frame(Opcode::Text, "\xff\xfe", true)), 1007);
	CHECK_EQ(close_code_for(encode_frame(Opcode::Continuation, "x", true)), 1002);
	CHECK_EQ(close_code_for(encode_frame(Opcode::Text, "a", true, false) + encode_frame(Opcode::Text, "b", true)), 1002);
	CHECK_EQ(close_code_for(encode_frame(Opcode::Close, "\x03", true)), 1002);							//1 バイトの close
	CHECK_EQ(close_code_for(encode_frame(Opcode::Close, std::string("\x03\xED", 2), true)), 1002);	//1005 は送ってはいけない
	CHECK_EQ(close_code_for(encode_frame(Opcode::Close, std::string("\x03\xE8\xff", 3), true)), 1007);
}

TEST(ws_empty_fragment_flood, "WebSocket: 空の継続フレームを大量に送られてもメモリを使わない") {
	EchoServer s;
	std::string frames = encode_frame(Opcode::Text, "a", true, false);
	for (int i = 0; i < 20000; ++i) frames += encode_frame(Opcode::Continuation, "", true, false);
	frames += encode_frame(Opcode::Continuation, "b", true, true);
	const auto replies = raw_session(s.port, frames);
	CHECK_EQ(replies.size(), 1u);
	if (!replies.empty()) CHECK_EQ(replies[0].payload, "echo:ab");
}

TEST(ws_connect_failure, "WebSocket: 接続できないときは error と close(1006) が届く") {
	LoopThread lt;
	const Listener l = listen_tcp("127.0.0.1", 0);
	close_socket(l.fd);	//誰も待っていないポート

	std::promise<int> closed;
	std::atomic<bool> errored{false};
	std::shared_ptr<WebSocket> client;
	lt.loop().dispatch_sync([&] {
		client = connect_websocket(lt.loop(), "ws://127.0.0.1:" + std::to_string(l.port) + "/");
		client->on_error = [&](const std::string&) { errored = true; };
		client->on_close = [&](int code, std::string_view) { closed.set_value(code); };
	});
	auto f = closed.get_future();
	CHECK(ready(f));
	CHECK_EQ(f.get(), 1006);
	CHECK(errored.load());
	CHECK_THROWS(connect_websocket(lt.loop(), "wss://example.com/"));
	CHECK_THROWS(connect_websocket(lt.loop(), "http://example.com/"));
}

TEST(ws_slow_consumer, "WebSocket: 読まない相手への送信待ちが上限を超えたら切断する") {
	WebSocketServerOptions options;
	options.max_buffered = 256 * 1024;
	LoopThread lt;
	WebSocketServer server(lt.loop(), options);
	std::promise<void> closed;
	server.on_connection = [&](const std::shared_ptr<WebSocket>& ws, const RequestHead&) {
		ws->on_close = [&](int, std::string_view) { closed.set_value(); };
		const std::string chunk(64 * 1024, 'x');
		for (int i = 0; i < 400; ++i) ws->send(chunk);	//25MiB。相手は読まない
	};
	const int port = server.listen(0);

	const socket_t fd = connect_tcp("127.0.0.1", port, 3000);
	const std::string request = std::string(kUpgrade) + "\r\n";
	socket_send(fd, request.data(), request.size());

	auto f = closed.get_future();
	CHECK(ready(f));
	close_socket(fd);
	server.close();
}
