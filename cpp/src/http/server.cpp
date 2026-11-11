#include "portus/http.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "portus/id.hpp"
#include "portus/tcp_conn.hpp"
#include "portus/thread_pool.hpp"
#include "portus/util.hpp"

namespace portus {

namespace {

using Clock = std::chrono::steady_clock;

const std::size_t kNoBufferLimit = static_cast<std::size_t>(-1);	//応答は 1 接続 1 つずつで、大きさはハンドラが決める
const int kLingerMs = 2000;
const int kAcceptRetryMs = 100;

//Writing: 応答を送り切るまで読み止めて待つ (読まずに要求だけ送り続ける相手に応答を溜め込まない)
enum class State { Head, Body, Handling, Writing, Closing };

struct Conn {
	std::uint64_t id = 0;
	std::shared_ptr<TcpConn> tcp;
	std::string remote;
	std::string in;
	State state = State::Head;
	RequestHead head;
	std::string req_id;
	std::size_t body_len = 0;
	bool keep_alive = true;
	bool receiving = false;	//要求の最初のバイトが届いたか
	Clock::time_point deadline;
	Clock::time_point started;
	std::uint64_t sent_mark = 0;		//送信待ちがある間、最後に送信が進んだ時点の送信量と時刻
	Clock::time_point sent_at;
};

bool has_body_method(const std::string& method) {
	return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
}

//Host ヘッダからポートを除いた名前 (小文字)
std::string host_name(std::string_view host) {
	if (!host.empty() && host[0] == '[') {
		const std::size_t end = host.find(']');
		return to_lower(host.substr(0, end == std::string_view::npos ? host.size() : end + 1));
	}
	return to_lower(host.substr(0, host.find(':')));
}

std::string path_of(const std::string& target) {
	return target.substr(0, target.find('?'));
}

//応答を 1 つのバイト列にする。ハンドラが入れたヘッダに改行などがあれば例外
std::string serialize(const Reply& reply, const std::string& req_id, bool keep_alive, bool head_only) {
	const bool no_body = reply.status == 204 || reply.status == 304;
	std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " " + status_text(reply.status) + "\r\n";
	for (const auto& kv : reply.headers) {
		const std::string name = to_lower(kv.first);
		if (!is_valid_header_name(name) || !is_valid_header_value(kv.second)) throw std::runtime_error("invalid response header " + name);
		if (name == "content-length" || name == "connection" || name == "transfer-encoding" || name == "x-request-id") continue;
		out += name + ": " + kv.second + "\r\n";
	}
	if (!req_id.empty()) out += "x-request-id: " + req_id + "\r\n";
	if (!no_body) out += "content-length: " + std::to_string(reply.body.size()) + "\r\n";
	out += keep_alive ? "connection: keep-alive\r\n\r\n" : "connection: close\r\n\r\n";
	if (!no_body && !head_only) out += reply.body;
	return out;
}

}

struct HttpServer::Impl : std::enable_shared_from_this<HttpServer::Impl> {
	EventLoop& loop;
	HttpServerOptions options;
	Logger log;

	mutable std::mutex router_mu;
	std::shared_ptr<const Router> router;

	std::unique_ptr<ThreadPool> pool;
	socket_t listener = kInvalidSocket;
	std::unordered_map<std::uint64_t, std::shared_ptr<Conn>> conns;
	std::uint64_t next_id = 1;
	std::uint64_t sweep_timer = 0;
	std::uint64_t accept_timer = 0;
	std::atomic<bool> closed{false};

	Impl(EventLoop& loop_, HttpServerOptions options_) : loop(loop_), options(std::move(options_)), log(options.log) {
		router = std::make_shared<const Router>(std::move(options.routes));
		options.routes.clear();
		if (options.worker_threads > 0) pool.reset(new ThreadPool(options.worker_threads, options.max_pending));
	}

	std::shared_ptr<const Router> current_router() const {
		std::lock_guard<std::mutex> lock(router_mu);
		return router;
	}

	//--- 接続の受け入れ ---

	void watch_listener() {
		std::shared_ptr<Impl> self = shared_from_this();
		loop.add(listener, kRead, [self](unsigned) { self->on_accept(); });
	}

	//accept が待ち以外で失敗した (fd の上限など)。待ち受けたままだと poll がすぐ戻り続けるので、少し外してから戻す
	void back_off_accept() {
		loop.remove(listener);
		std::weak_ptr<Impl> weak = shared_from_this();
		accept_timer = loop.set_timer(kAcceptRetryMs, [weak] {
			auto self = weak.lock();
			if (!self) return;
			self->accept_timer = 0;
			if (self->listener != kInvalidSocket) self->watch_listener();
		});
	}

	void on_accept() {
		for (;;) {
			std::string peer;
			const socket_t fd = accept_tcp(listener, &peer);
			if (fd == kInvalidSocket) {
				if (!last_error_would_block()) back_off_accept();
				return;
			}
			if (conns.size() >= options.max_connections) {
				close_socket(fd);
				continue;
			}

			auto conn = std::make_shared<Conn>();
			conn->id = next_id++;
			conn->remote = peer;
			conn->deadline = Clock::now() + std::chrono::milliseconds(options.request_timeout_ms);
			conn->tcp = std::make_shared<TcpConn>(loop, fd, kNoBufferLimit);

			std::weak_ptr<Impl> weak = shared_from_this();
			const std::uint64_t id = conn->id;
			conn->tcp->set_on_data([weak, id](const char* data, std::size_t len) {
				if (auto self = weak.lock()) self->on_data(id, data, len);
			});
			conn->tcp->set_on_close([weak, id] {
				if (auto self = weak.lock()) self->conns.erase(id);
			});
			conn->tcp->set_on_drain([weak, id] {
				if (auto self = weak.lock()) self->on_drain(id);
			});
			conns[id] = conn;
			conn->tcp->start();
		}
	}

	std::shared_ptr<Conn> find(std::uint64_t id) {
		auto it = conns.find(id);
		return it == conns.end() ? nullptr : it->second;
	}

	//--- 要求の読み取り ---

	void on_data(std::uint64_t id, const char* data, std::size_t len) {
		std::shared_ptr<Conn> conn = find(id);
		if (!conn || conn->state == State::Handling || conn->state == State::Writing || conn->state == State::Closing) return;

		if (!conn->receiving) {
			conn->receiving = true;
			conn->deadline = Clock::now() + std::chrono::milliseconds(options.request_timeout_ms);
		}
		conn->in.append(data, len);
		process(conn);
	}

	//溜まっている入力から要求を取り出して処理する。同じ接続で続けて届いた分も、スタックを深くせずに順に進める
	void process(const std::shared_ptr<Conn>& conn) {
		for (;;) {
			if (conn->tcp->closed()) return;

			if (conn->state == State::Head) {
				if (conn->in.empty()) return;
				conn->receiving = true;

				std::size_t consumed = 0;
				conn->head = RequestHead();
				const ParseResult r = parse_request_head(conn->in, options.max_header_bytes, conn->head, consumed);
				if (r == ParseResult::Incomplete) return;

				conn->started = Clock::now();
				conn->req_id = uuidv4();
				if (r == ParseResult::Bad) {
					const std::size_t end = conn->in.find("\r\n\r\n");
					const bool too_large = end == std::string::npos || end + 4 > options.max_header_bytes;
					conn->head = RequestHead();
					reject(conn, too_large ? 431 : 400, too_large ? "header too large." : "bad request.");
					return;
				}
				conn->in.erase(0, consumed);
				if (!accept_head(conn)) return;
				conn->state = State::Body;
			}

			if (conn->state != State::Body || conn->in.size() < conn->body_len) return;
			std::string body = conn->in.substr(0, conn->body_len);
			conn->in.erase(0, conn->body_len);
			if (!dispatch(conn, std::move(body))) return;	//応答待ち、または接続を閉じる
		}
	}

	//ヘッダを見て受けるかどうか決める。受けないときは応答を返して false
	bool accept_head(const std::shared_ptr<Conn>& conn) {
		const Headers& h = conn->head.headers;
		conn->keep_alive = conn->head.minor_version == 1
			? !header_has_token(h.get("connection"), "close")
			: header_has_token(h.get("connection"), "keep-alive");

		//chunked は受けない。長さの解釈が前段のプロキシと食い違うと、要求を紛れ込まされる
		if (h.find("transfer-encoding")) {
			reject(conn, 411, "length required.");
			return false;
		}
		std::uint64_t length = 0;
		if (!parse_content_length(h, length)) {
			reject(conn, 400, "bad request.");
			return false;
		}
		if (!options.allowed_hosts.empty()) {
			const std::string host = host_name(h.get("host"));
			bool allowed = false;
			for (const std::string& a : options.allowed_hosts) allowed = allowed || to_lower(a) == host;
			if (!allowed) {
				reject(conn, 421, "misdirected request.");
				return false;
			}
		}
		if (length > options.max_body_bytes) {
			reject(conn, 413, "payload too large.");
			return false;
		}

		conn->body_len = static_cast<std::size_t>(length);
		if (length > 0 && header_has_token(h.get("expect"), "100-continue")) conn->tcp->write("HTTP/1.1 100 Continue\r\n\r\n");
		return true;
	}

	//--- ハンドラの実行 ---

	Reply call_handler(RequestContext& ctx) const {
		const RouteDef& def = *ctx.route;
		if (def.handler) return def.handler(ctx);
		const auto it = options.handlers.find(def.handler_ref);
		if (it == options.handlers.end() || !it->second) return text("handler not found.", 404);
		return it->second(ctx);
	}

	//ミドルウェアを外側から順に重ね、最後にハンドラを呼ぶ
	Reply run_pipeline(RequestContext& ctx, std::size_t index) const {
		if (index >= options.middlewares.size()) return call_handler(ctx);
		bool called = false;
		return options.middlewares[index](ctx, [&]() -> Reply {
			if (called) throw std::logic_error("next() called multiple times");
			called = true;
			return run_pipeline(ctx, index + 1);
		});
	}

	//例外を 500 に変えてパイプラインを実行する
	Reply run_guarded(RequestContext& ctx, const std::string& handler) const {
		try {
			return run_pipeline(ctx, 0);
		} catch (const std::exception& ex) {
			log_failure(ctx, handler, ex.what());
		} catch (...) {
			log_failure(ctx, handler, "unknown exception");
		}
		return text("internal error.", 500);
	}

	//戻り値: 応答まで済んで、同じ接続の次の要求へ進めるか
	bool dispatch(const std::shared_ptr<Conn>& conn, std::string body) {
		conn->state = State::Handling;
		conn->tcp->pause_read(true);

		const std::string path = path_of(conn->head.target);
		std::optional<RouteMatch> match = current_router()->match(conn->head.method, conn->head.target);
		if (!match) return respond(conn, text("not found.", 404), "-");

		auto ctx = std::make_shared<RequestContext>();
		ctx->req_id = conn->req_id;
		ctx->method = conn->head.method;
		ctx->path = path;
		ctx->remote_addr = conn->remote;
		ctx->headers = conn->head.headers;
		ctx->route = match->def;
		ctx->params = std::move(match->params);
		ctx->query = std::move(match->query);
		if (has_body_method(ctx->method)) ctx->body = std::move(body);
		ctx->log = log.child("req", Json::object({ { "reqId", Json(ctx->req_id) } }));

		const std::string handler = match->def->handler_name();
		//ワーカー無し: ループスレッドでそのまま実行する (ハンドラがブロックしない前提の構成)
		if (!pool) {
			merge_request_body(*ctx);
			return respond(conn, run_guarded(*ctx, handler), handler);
		}

		std::shared_ptr<Impl> self = shared_from_this();
		const std::uint64_t id = conn->id;
		const bool queued = pool->submit([self, ctx, id, handler] {
			//ボディの解析は大きいと重いので、ループではなくワーカーで行う
			self->merge_request_body(*ctx);
			auto reply = std::make_shared<Reply>(self->run_guarded(*ctx, handler));
			//ハンドラの中で close されていたら、ループはもう使えないかもしれない (応答を受ける接続も残っていない)
			if (self->closed.load()) return;
			self->loop.post([self, id, reply, handler] {
				std::shared_ptr<Conn> c = self->find(id);
				if (c && self->respond(c, std::move(*reply), handler)) self->process(c);
			});
		});
		if (!queued) return respond(conn, text("server busy.", 503), handler);
		return false;
	}

	static void merge_request_body(RequestContext& ctx) {
		if (!ctx.body.empty()) merge_body(ctx.query, ctx.body, ctx.params);
	}

	void log_failure(const RequestContext& ctx, const std::string& handler, const std::string& what) const {
		log.error("handler failed", Json::object({
			{ "reqId", Json(ctx.req_id) }, { "method", Json(ctx.method) }, { "path", Json(ctx.path) }, { "handler", Json(handler) }, { "err", Json(what) },
		}));
	}

	//--- 応答 ---

	//戻り値: 接続を続け、次の要求を待つ状態に戻したか
	bool respond(const std::shared_ptr<Conn>& conn, Reply reply, const std::string& handler) {
		const bool keep = conn->keep_alive && !closed;
		const bool head_only = conn->head.method == "HEAD";
		std::string bytes;
		try {
			bytes = serialize(reply, conn->req_id, keep, head_only);
		} catch (const std::exception& ex) {
			log.error("handler failed", Json::object({ { "reqId", Json(conn->req_id) }, { "handler", Json(handler) }, { "err", Json(ex.what()) } }));
			reply = text("internal error.", 500);
			bytes = serialize(reply, conn->req_id, keep, head_only);
		}
		access_log(*conn, handler, reply.status);
		conn->tcp->write(bytes);
		if (conn->tcp->closed()) return false;

		if (!keep) {
			linger(conn);
			return false;
		}
		if (conn->tcp->buffered() > 0) {
			//送り切るまで次の要求は読まない。続きは on_drain から
			conn->state = State::Writing;
			mark_sent(*conn);
			return false;
		}
		wait_next(conn);
		return true;
	}

	//応答を送り切った。keep-alive の待ち時間はここから数える
	void wait_next(const std::shared_ptr<Conn>& conn) {
		conn->state = State::Head;
		conn->receiving = false;
		conn->deadline = Clock::now() + std::chrono::milliseconds(options.keep_alive_timeout_ms);
		conn->tcp->pause_read(false);
	}

	void on_drain(std::uint64_t id) {
		std::shared_ptr<Conn> conn = find(id);
		if (!conn) return;
		if (conn->state == State::Closing) {
			conn->deadline = Clock::now() + std::chrono::milliseconds(kLingerMs);
			return;
		}
		if (conn->state != State::Writing) return;
		wait_next(conn);
		process(conn);	//送り切るのを待つ間に届いていた (先読み済みの) 要求を進める
	}

	static void mark_sent(Conn& conn) {
		conn.sent_mark = conn.tcp->sent_bytes();
		conn.sent_at = Clock::now();
	}

	//要求を最後まで読まずに返す応答 (400 / 411 / 413 / 421 / 431)。接続は続けない
	void reject(const std::shared_ptr<Conn>& conn, int status, const char* message) {
		conn->keep_alive = false;
		std::string bytes = serialize(text(message, status), conn->req_id, false, false);
		access_log(*conn, "-", status);
		conn->tcp->write(bytes);
		if (!conn->tcp->closed()) linger(conn);
	}

	//応答を送り切ってから閉じる。相手が送信中のボディは少しの間だけ読み捨てる (すぐ閉じると応答が届く前にリセットされる)
	void linger(const std::shared_ptr<Conn>& conn) {
		conn->state = State::Closing;
		conn->in.clear();
		conn->deadline = Clock::now() + std::chrono::milliseconds(kLingerMs);	//送り切ったら on_drain で数え直す
		mark_sent(*conn);
		conn->tcp->end();
	}

	//1 リクエスト 1 行。ボディとクエリは出さない
	void access_log(const Conn& conn, const std::string& handler, int status) const {
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - conn.started).count();
		log.info("access", Json::object({
			{ "reqId", Json(conn.req_id) }, { "method", Json(conn.head.method) }, { "path", Json(path_of(conn.head.target)) },
			{ "handler", Json(handler) }, { "status", Json(status) }, { "ms", Json(ms) },
		}));
	}

	//期限を過ぎた接続を閉じる (ハンドラ実行中は対象外)
	//  応答の送信待ちがある間は期限で切らない。遅くても受け取っている相手には最後まで送り、write_timeout_ms の間まったく進まなければ切る
	void sweep() {
		const auto now = Clock::now();
		const auto stall = std::chrono::milliseconds(options.write_timeout_ms);
		std::vector<std::shared_ptr<Conn>> expired;
		for (const auto& kv : conns) {
			Conn& c = *kv.second;
			if (c.state == State::Handling) continue;
			if (c.state == State::Writing || c.tcp->buffered() > 0) {
				if (c.tcp->sent_bytes() != c.sent_mark) mark_sent(c);
				else if (now - c.sent_at >= stall) expired.push_back(kv.second);
				continue;
			}
			if (c.deadline <= now) expired.push_back(kv.second);
		}
		for (const auto& conn : expired) conn->tcp->close();
	}

	void shutdown() {
		if (listener != kInvalidSocket) {
			loop.remove(listener);
			close_socket(listener);
			listener = kInvalidSocket;
		}
		if (sweep_timer) loop.cancel_timer(sweep_timer);
		sweep_timer = 0;
		if (accept_timer) loop.cancel_timer(accept_timer);
		accept_timer = 0;

		std::vector<std::shared_ptr<Conn>> all;
		for (const auto& kv : conns) all.push_back(kv.second);
		for (const auto& conn : all) conn->tcp->close();
		conns.clear();
	}
};

HttpServer::HttpServer(EventLoop& loop, HttpServerOptions options) : impl_(std::make_shared<Impl>(loop, std::move(options))) {}

HttpServer::~HttpServer() {
	close();
}

int HttpServer::listen(int port, const std::string& host) {
	const Listener l = listen_tcp(host, port);
	std::shared_ptr<Impl> impl = impl_;
	impl->loop.dispatch_sync([impl, l] {
		impl->listener = l.fd;
		impl->watch_listener();
		const HttpServerOptions& o = impl->options;
		const int shortest = std::min({ o.request_timeout_ms, o.keep_alive_timeout_ms, o.write_timeout_ms });
		const int interval = shortest / 4 < 20 ? 20 : (shortest / 4 > 500 ? 500 : shortest / 4);
		impl->sweep_timer = impl->loop.set_timer(interval, [impl] { impl->sweep(); }, true);
	});
	impl->log.info("http server listening", Json::object({ { "host", Json(host) }, { "port", Json(l.port) }, { "routes", Json(impl->current_router()->list().size()) } }));
	return l.port;
}

void HttpServer::set_routes(std::vector<RouteDef> routes) {
	auto next = std::make_shared<const Router>(std::move(routes));
	std::lock_guard<std::mutex> lock(impl_->router_mu);
	impl_->router = std::move(next);
}

std::vector<RouteDef> HttpServer::routes() const {
	return impl_->current_router()->list();
}

void HttpServer::close() {
	std::shared_ptr<Impl> impl = impl_;
	if (impl->closed.exchange(true)) return;
	impl->loop.dispatch_sync([impl] { impl->shutdown(); });
	impl->pool.reset();	//実行中のハンドラが終わるのを待つ
}

}
