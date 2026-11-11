#include "portus/ws.hpp"

#include <chrono>
#include <unordered_map>

#include "portus/tcp_conn.hpp"

namespace portus {

namespace {

using Clock = std::chrono::steady_clock;
const std::size_t kMaxHandshakeBytes = 16 * 1024;

//ハンドシェイク待ちの接続
struct Pending {
	std::shared_ptr<TcpConn> tcp;
	std::string in;
	Clock::time_point deadline;
	bool answered = false;
};

}

struct WebSocketServer::Impl : std::enable_shared_from_this<WebSocketServer::Impl> {
	WebSocketServer& owner;
	EventLoop& loop;
	WebSocketServerOptions options;
	socket_t listener = kInvalidSocket;
	std::unordered_map<std::uint64_t, Pending> pending;
	std::vector<std::weak_ptr<WebSocket>> sockets;
	std::uint64_t next_id = 1;
	std::uint64_t sweep_timer = 0;

	Impl(WebSocketServer& owner_, EventLoop& loop_, WebSocketServerOptions options_) : owner(owner_), loop(loop_), options(std::move(options_)) {}

	std::size_t live_sockets() {
		std::vector<std::weak_ptr<WebSocket>> alive;
		for (const auto& weak : sockets) {
			const auto ws = weak.lock();
			if (ws && ws->ready_state() != WebSocket::CLOSED) alive.push_back(weak);
		}
		sockets.swap(alive);
		return sockets.size();
	}

	void on_accept() {
		for (;;) {
			const socket_t fd = accept_tcp(listener, nullptr);
			if (fd == kInvalidSocket) return;
			if (pending.size() + live_sockets() >= options.max_connections) {
				close_socket(fd);
				continue;
			}

			const std::uint64_t id = next_id++;
			Pending& p = pending[id];
			p.deadline = Clock::now() + std::chrono::milliseconds(options.handshake_timeout_ms);
			p.tcp = std::make_shared<TcpConn>(loop, fd, options.max_buffered);

			std::weak_ptr<Impl> weak = shared_from_this();
			p.tcp->set_on_data([weak, id](const char* data, std::size_t len) {
				if (auto self = weak.lock()) self->on_data(id, data, len);
			});
			p.tcp->set_on_close([weak, id] {
				if (auto self = weak.lock()) self->pending.erase(id);
			});
			p.tcp->start();
		}
	}

	//応答を返して閉じる (ハンドシェイクが成立しなかったとき)
	void refuse(Pending& p, int status, const std::string& body) {
		std::shared_ptr<TcpConn> tcp = p.tcp;	//書き込みで切れると p は消える
		p.answered = true;
		p.deadline = Clock::now() + std::chrono::milliseconds(2000);
		tcp->write("HTTP/1.1 " + std::to_string(status) + " " + status_text(status)
			+ "\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
		tcp->end();
	}

	void on_data(std::uint64_t id, const char* data, std::size_t len) {
		auto it = pending.find(id);
		if (it == pending.end() || it->second.answered) return;
		Pending& p = it->second;
		p.in.append(data, len);

		RequestHead head;
		std::size_t consumed = 0;
		const ParseResult r = parse_request_head(p.in, kMaxHandshakeBytes, head, consumed);
		if (r == ParseResult::Incomplete) return;
		if (r == ParseResult::Bad) return refuse(p, 400, "bad request");

		if (!head.headers.find("upgrade")) return refuse(p, 426, "Upgrade Required");
		if (const std::optional<std::string> error = validate_upgrade(head.method, head.headers)) return refuse(p, 400, *error);

		const std::string* origin = head.headers.find("origin");
		if (origin && !options.allowed_origins.empty()) {
			bool allowed = false;
			for (const std::string& a : options.allowed_origins) allowed = allowed || a == *origin;
			if (!allowed) return refuse(p, 403, "origin not allowed");
		}

		std::shared_ptr<TcpConn> tcp = p.tcp;
		const std::string rest = p.in.substr(consumed);
		pending.erase(it);

		tcp->write(build_accept_response(head.headers.get("sec-websocket-key")));
		if (tcp->closed()) return;

		auto ws = std::make_shared<WebSocket>(loop, false, options.max_payload);
		ws->attach(tcp);
		sockets.push_back(ws);
		//コールバックを設定してもらってから、先読み分のフレームを渡す
		if (owner.on_connection) owner.on_connection(ws, head);
		ws->receive(rest);
	}

	void sweep() {
		const auto now = Clock::now();
		std::vector<std::shared_ptr<TcpConn>> expired;
		for (const auto& kv : pending) {
			if (kv.second.deadline <= now) expired.push_back(kv.second.tcp);
		}
		for (const auto& tcp : expired) tcp->close();
	}

	void shutdown() {
		if (listener != kInvalidSocket) {
			loop.remove(listener);
			close_socket(listener);
			listener = kInvalidSocket;
		}
		if (sweep_timer) loop.cancel_timer(sweep_timer);
		sweep_timer = 0;

		std::vector<std::shared_ptr<TcpConn>> conns;
		for (const auto& kv : pending) conns.push_back(kv.second.tcp);
		for (const auto& tcp : conns) tcp->close();
		pending.clear();

		std::vector<std::weak_ptr<WebSocket>> all;
		all.swap(sockets);
		for (const auto& weak : all) {
			if (auto ws = weak.lock()) ws->terminate();
		}
	}
};

WebSocketServer::WebSocketServer(EventLoop& loop, WebSocketServerOptions options) : impl_(std::make_shared<Impl>(*this, loop, std::move(options))) {}

WebSocketServer::~WebSocketServer() {
	close();
}

int WebSocketServer::listen(int port, const std::string& host) {
	const Listener l = listen_tcp(host, port);
	std::shared_ptr<Impl> impl = impl_;
	impl->loop.dispatch_sync([impl, l] {
		impl->listener = l.fd;
		impl->loop.add(l.fd, kRead, [impl](unsigned) { impl->on_accept(); });
		impl->sweep_timer = impl->loop.set_timer(250, [impl] { impl->sweep(); }, true);
	});
	return l.port;
}

void WebSocketServer::close() {
	std::shared_ptr<Impl> impl = impl_;
	impl->loop.dispatch_sync([impl] { impl->shutdown(); });
}

}
