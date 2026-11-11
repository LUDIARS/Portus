#include "portus/ws.hpp"

#include "portus/tcp_conn.hpp"
#include "portus/url.hpp"

namespace portus {

namespace {

const int kConnectTimeoutMs = 10000;
const std::size_t kMaxHandshakeBytes = 16 * 1024;

//接続 → ハンドシェイクまでを進める。候補のアドレスを順に試す
struct Connector : std::enable_shared_from_this<Connector> {
	EventLoop& loop;
	std::shared_ptr<WebSocket> ws;
	Url url;
	std::vector<SockAddr> addrs;
	std::size_t index = 0;
	socket_t fd = kInvalidSocket;
	std::shared_ptr<TcpConn> tcp;
	std::string key;
	std::string in;
	std::uint64_t timer = 0;
	bool done = false;

	Connector(EventLoop& loop_, std::shared_ptr<WebSocket> ws_, Url url_) : loop(loop_), ws(std::move(ws_)), url(std::move(url_)) {}

	void start() {
		try {
			addrs = resolve_tcp(url.host, url.port);
		} catch (const std::exception& ex) {
			return fail(ex.what());
		}
		std::shared_ptr<Connector> self = shared_from_this();
		timer = loop.set_timer(kConnectTimeoutMs, [self] {
			self->timer = 0;
			self->fail("connect timeout");
		});
		try_next();
	}

	void try_next() {
		std::shared_ptr<Connector> self = shared_from_this();
		while (index < addrs.size()) {
			bool in_progress = false;
			fd = start_connect(addrs[index++], &in_progress);
			if (fd == kInvalidSocket) continue;
			if (!in_progress) return connected();

			loop.add(fd, kWrite, [self](unsigned) {
				const socket_t s = self->fd;
				self->loop.remove(s);
				if (self->done) return close_socket(s);
				if (socket_error(s) == 0) return self->connected();
				close_socket(s);
				self->fd = kInvalidSocket;
				self->try_next();
			});
			return;
		}
		fail("cannot connect to " + url.host_header());
	}

	void connected() {
		std::shared_ptr<Connector> self = shared_from_this();
		tcp = std::make_shared<TcpConn>(loop, fd, kDefaultMaxBuffered);
		fd = kInvalidSocket;
		tcp->set_on_data([self](const char* data, std::size_t len) { self->on_data(data, len); });
		tcp->set_on_close([self] { self->fail("connection closed during handshake"); });
		tcp->start();

		key = generate_key();
		tcp->write("GET " + url.target + " HTTP/1.1\r\nHost: " + url.host_header()
			+ "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n");
	}

	void on_data(const char* data, std::size_t len) {
		if (done) return;
		in.append(data, len);

		ResponseHead head;
		std::size_t consumed = 0;
		const ParseResult r = parse_response_head(in, kMaxHandshakeBytes, head, consumed);
		if (r == ParseResult::Incomplete) return;
		if (r == ParseResult::Bad) return fail("invalid handshake response");
		if (head.status != 101) return fail("unexpected response " + std::to_string(head.status));
		if (head.headers.get("sec-websocket-accept") != accept_key(key)) return fail("invalid Sec-WebSocket-Accept");

		done = true;
		if (timer) loop.cancel_timer(timer);
		timer = 0;
		const std::string rest = in.substr(consumed);
		std::shared_ptr<WebSocket> socket = ws;
		std::shared_ptr<TcpConn> conn = std::move(tcp);
		ws.reset();

		socket->attach(conn);	//ここで conn のコールバックが WebSocket のものに替わる
		if (socket->ready_state() != WebSocket::OPEN) return;
		if (socket->on_open) socket->on_open();
		socket->receive(rest);
	}

	void fail(const std::string& message) {
		if (done) return;
		done = true;
		if (timer) loop.cancel_timer(timer);
		timer = 0;
		if (fd != kInvalidSocket) {
			loop.remove(fd);
			close_socket(fd);
			fd = kInvalidSocket;
		}
		std::shared_ptr<TcpConn> conn = std::move(tcp);
		if (conn) conn->close();
		std::shared_ptr<WebSocket> socket = std::move(ws);
		if (socket) socket->fail_connect(message);
	}
};

}

std::shared_ptr<WebSocket> connect_websocket(EventLoop& loop, const std::string& url_text, std::size_t max_payload) {
	Url url;
	if (!parse_url(url_text, url)) throw std::invalid_argument("invalid url");
	if (url.scheme == "wss") throw std::invalid_argument("wss:// is not supported (terminate TLS at a reverse proxy)");
	if (url.scheme != "ws") throw std::invalid_argument("unsupported protocol " + url.scheme);

	auto ws = std::make_shared<WebSocket>(loop, true, max_payload);
	auto connector = std::make_shared<Connector>(loop, ws, std::move(url));
	//必ず後で始める。呼び出し側が戻り値へコールバックを設定する間に事象が起きないようにする
	loop.post([connector] { connector->start(); });
	return ws;
}

}
