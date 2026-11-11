#include "portus/ws.hpp"

#include <atomic>

#include "portus/tcp_conn.hpp"
#include "portus/util.hpp"

namespace portus {

namespace {

const int kCloseWaitMs = 5000;

//相手から受け取ってよい close コードか (RFC 6455 7.4)
bool valid_close_code(int code) {
	return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

std::string close_payload(int code, std::string_view reason) {
	//制御フレームは 125 バイトまで。理由は UTF-8 の文字の途中で切らない
	std::size_t len = reason.size() > 123 ? 123 : reason.size();
	while (len > 0 && len < reason.size() && (static_cast<unsigned char>(reason[len]) & 0xC0) == 0x80) --len;

	std::string out;
	out.push_back(static_cast<char>((code >> 8) & 0xFF));
	out.push_back(static_cast<char>(code & 0xFF));
	out.append(reason.substr(0, len));
	return out;
}

}

struct WebSocket::Impl {
	WebSocket& owner;
	EventLoop& loop;
	bool is_client;
	std::size_t max_payload;
	FrameParser parser;

	std::atomic<int> state{WebSocket::CONNECTING};
	std::shared_ptr<TcpConn> conn;
	std::string message;	//分割メッセージの組み立て先 (断片の個数に関係なく 1 本)
	int message_opcode = -1;
	int close_code = 1006;
	std::string close_reason;
	std::uint64_t close_timer = 0;

	Impl(WebSocket& owner_, EventLoop& loop_, bool is_client_, std::size_t max_payload_)
		: owner(owner_), loop(loop_), is_client(is_client_), max_payload(max_payload_), parser(!is_client_, max_payload_) {}

	void write(Opcode opcode, std::string_view payload) {
		if (conn) conn->write(encode_frame(opcode, payload, is_client));
	}

	//コールバックは手元に写してから呼ぶ。呼ばれた側が ws->on_xxx を差し替えると、実行中の関数 (と掴んだ値) が壊れるため
	void emit_error(const std::string& message_text) {
		if (auto cb = owner.on_error) cb(message_text);
	}

	//相手が閉じるのを待つ。応答が無くても時間で切る
	void end_and_wait() {
		if (conn) conn->end();
		if (close_timer) return;
		std::weak_ptr<WebSocket> weak = owner.weak_from_this();
		close_timer = loop.set_timer(kCloseWaitMs, [weak] {
			if (auto ws = weak.lock()) {
				ws->impl_->close_timer = 0;
				ws->terminate();
			}
		});
	}

	void on_data(std::string_view chunk) {
		try {
			for (const Frame& frame : parser.push(chunk)) {
				if (state.load() == WebSocket::CLOSED) return;
				on_frame(frame);
			}
		} catch (const ProtocolError& ex) {
			fail_connection(ex.close_code, ex.what());
		}
	}

	void on_frame(const Frame& frame) {
		const int opcode = static_cast<int>(frame.opcode);
		if (opcode & 0x8) return on_control(frame);
		if (state.load() != WebSocket::OPEN) return;	//close を送った後のデータは捨てる

		if (frame.opcode == Opcode::Continuation) {
			if (message_opcode < 0) throw ProtocolError("unexpected continuation", 1002);
		} else {
			if (message_opcode >= 0) throw ProtocolError("expected continuation", 1002);
			message_opcode = opcode;
		}
		if (message.size() + frame.payload.size() > max_payload) throw ProtocolError("message too large", 1009);
		message.append(frame.payload);
		if (!frame.fin) return;

		std::string data;
		data.swap(message);
		const bool is_binary = message_opcode == static_cast<int>(Opcode::Binary);
		message_opcode = -1;

		if (!is_binary && !utf8_valid(data)) throw ProtocolError("invalid utf-8", 1007);
		if (auto cb = owner.on_message) cb(data, is_binary);
	}

	void on_control(const Frame& frame) {
		switch (frame.opcode) {
		case Opcode::Ping:
			if (state.load() == WebSocket::OPEN) write(Opcode::Pong, frame.payload);
			if (auto cb = owner.on_ping) cb(frame.payload);
			break;

		case Opcode::Pong:
			if (auto cb = owner.on_pong) cb(frame.payload);
			break;

		case Opcode::Close: {
			const std::string& p = frame.payload;
			if (p.size() == 1) throw ProtocolError("invalid close frame", 1002);
			if (p.size() >= 2) {
				const int code = (static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1]);
				if (!valid_close_code(code)) throw ProtocolError("invalid close code", 1002);
				if (!utf8_valid(std::string_view(p).substr(2))) throw ProtocolError("invalid utf-8 in close reason", 1007);
				close_code = code;
				close_reason = p.substr(2);
			} else {
				close_code = 1005;
			}
			if (state.load() == WebSocket::OPEN) {
				//受けた close をそのまま返して閉じる
				write(Opcode::Close, std::string_view(p).substr(0, p.size() >= 2 ? 2 : 0));
				state.store(WebSocket::CLOSING);
			}
			end_and_wait();
			break;
		}
		default:
			break;
		}
	}

	void fail_connection(int code, const std::string& message_text) {
		emit_error(message_text);
		if (state.load() == WebSocket::OPEN) {
			write(Opcode::Close, close_payload(code, ""));
			state.store(WebSocket::CLOSING);
		}
		close_code = code;
		end_and_wait();
	}

	void finish(int code, const std::string& reason) {
		if (state.exchange(WebSocket::CLOSED) == WebSocket::CLOSED) return;
		if (close_timer) loop.cancel_timer(close_timer);
		close_timer = 0;
		conn.reset();
		message.clear();
		if (auto cb = owner.on_close) cb(code, reason);

		//コールバックが ws 自身を掴んでいても解放されるよう、後で外す (実行中のコールバックを壊さないため今は外さない)
		std::shared_ptr<WebSocket> self = owner.shared_from_this();
		loop.post([self] {
			self->on_open = nullptr;
			self->on_message = nullptr;
			self->on_ping = nullptr;
			self->on_pong = nullptr;
			self->on_close = nullptr;
			self->on_error = nullptr;
		});
	}
};

WebSocket::WebSocket(EventLoop& loop, bool is_client, std::size_t max_payload) : impl_(new Impl(*this, loop, is_client, max_payload)) {}

WebSocket::~WebSocket() = default;

WebSocket::State WebSocket::ready_state() const {
	return static_cast<State>(impl_->state.load());
}

void WebSocket::attach(std::shared_ptr<TcpConn> conn) {
	if (impl_->state.load() != CONNECTING) {
		conn->close();
		return;
	}
	impl_->conn = conn;
	impl_->state.store(OPEN);

	std::shared_ptr<WebSocket> self = shared_from_this();
	conn->set_on_data([self](const char* data, std::size_t len) { self->impl_->on_data(std::string_view(data, len)); });
	conn->set_on_close([self] { self->impl_->finish(self->impl_->close_code, self->impl_->close_reason); });
}

void WebSocket::receive(std::string_view data) {
	if (!data.empty() && impl_->conn) impl_->on_data(data);
}

void WebSocket::fail_connect(const std::string& message) {
	if (impl_->state.load() == CLOSED) return;
	impl_->emit_error(message);
	impl_->finish(1006, "");
}

void WebSocket::send(std::string_view text) {
	std::shared_ptr<WebSocket> self = shared_from_this();
	std::string data(text);
	impl_->loop.dispatch([self, data] {
		if (self->impl_->state.load() == OPEN) self->impl_->write(Opcode::Text, data);
	});
}

void WebSocket::send_binary(std::string_view data_view) {
	std::shared_ptr<WebSocket> self = shared_from_this();
	std::string data(data_view);
	impl_->loop.dispatch([self, data] {
		if (self->impl_->state.load() == OPEN) self->impl_->write(Opcode::Binary, data);
	});
}

void WebSocket::ping(std::string_view data_view) {
	std::shared_ptr<WebSocket> self = shared_from_this();
	std::string data(data_view.substr(0, 125));
	impl_->loop.dispatch([self, data] {
		if (self->impl_->state.load() == OPEN) self->impl_->write(Opcode::Ping, data);
	});
}

void WebSocket::close(int code, std::string_view reason_view) {
	std::shared_ptr<WebSocket> self = shared_from_this();
	std::string reason(reason_view);
	impl_->loop.dispatch([self, code, reason] {
		Impl& s = *self->impl_;
		if (s.state.load() != OPEN) return;
		//送ってよいコード以外は正常終了として送る
		const bool sendable = code == 1000 || code == 1001 || (code >= 3000 && code <= 4999) || (code >= 1002 && code <= 1003) || (code >= 1007 && code <= 1011);
		s.write(Opcode::Close, close_payload(sendable ? code : 1000, reason));
		s.state.store(CLOSING);
		//相手の close が来たら end、来なくても時間で切る
		std::weak_ptr<WebSocket> weak = self;
		if (!s.close_timer) {
			s.close_timer = s.loop.set_timer(kCloseWaitMs, [weak] {
				if (auto ws = weak.lock()) {
					ws->impl_->close_timer = 0;
					ws->terminate();
				}
			});
		}
	});
}

void WebSocket::terminate() {
	std::shared_ptr<WebSocket> self = shared_from_this();
	impl_->loop.dispatch([self] {
		Impl& s = *self->impl_;
		if (s.state.load() == CLOSED) return;
		if (s.conn) {
			std::shared_ptr<TcpConn> conn = s.conn;
			conn->close();	//on_close 経由で finish が呼ばれる
		} else {
			s.finish(1006, "");
		}
	});
}

}
