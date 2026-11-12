#include "portus/mqtt.hpp"

#include <chrono>
#include <map>

#include "portus/tcp_conn.hpp"
#include "portus/url.hpp"
#include "portus/util.hpp"

namespace portus {

namespace {

//送るのは CONNECT / SUBSCRIBE / PUBACK / PINGREQ / DISCONNECT だけ。これだけ溜まるのは相手が読んでいないとき
const std::size_t kMaxBuffered = 1024 * 1024;
const int kDisconnectWaitMs = 2000;	//DISCONNECT の後、ブローカーが閉じるのを待つ上限

std::int64_t steady_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* refusal_reason(int code) {
	switch (code) {
	case 1: return "unacceptable protocol version";
	case 2: return "identifier rejected";
	case 3: return "server unavailable";
	case 4: return "bad user name or password";
	case 5: return "not authorized";
	default: return "unknown";
	}
}

enum class State { Idle, Connecting, WaitConnack, Connected, Waiting, Ended };

}

struct MqttClient::Impl {
	MqttClient& owner;
	EventLoop& loop;
	MqttOptions options;
	Url url;
	std::string connect_packet;	//create で組み立てて検証済み。接続ごとに同じものを送る
	int keep_alive_ms;

	State state = State::Idle;
	std::uint64_t gen = 0;	//試行ごとに進める。古い試行のソケット・タイマーのコールバックはこれで捨てる
	std::vector<SockAddr> addrs;
	std::size_t addr_index = 0;
	socket_t pending_fd = kInvalidSocket;
	std::shared_ptr<TcpConn> tcp;
	MqttPacketParser parser;
	std::uint64_t connect_timer = 0;
	std::uint64_t keep_timer = 0;
	std::uint64_t reconnect_timer = 0;
	std::int64_t last_send = 0;
	std::int64_t last_recv = 0;
	std::map<std::uint16_t, std::function<void(bool)>> pending_subs;
	std::uint16_t next_packet_id = 0;

	Impl(MqttClient& owner_, EventLoop& loop_, MqttOptions options_, Url url_, std::string connect_packet_)
		: owner(owner_), loop(loop_), options(std::move(options_)), url(std::move(url_)), connect_packet(std::move(connect_packet_)),
		keep_alive_ms(options.keep_alive_sec * 1000), parser(options.max_packet_bytes) {}

	//手放されたときの後始末。ループの資源はループスレッドで外す (this は使わない)
	~Impl() {
		EventLoop* l = &loop;
		std::shared_ptr<TcpConn> conn = std::move(tcp);
		const socket_t fd = pending_fd;
		const std::uint64_t t1 = connect_timer, t2 = keep_timer, t3 = reconnect_timer;
		l->dispatch([l, conn, fd, t1, t2, t3] {
			if (t1) l->cancel_timer(t1);
			if (t2) l->cancel_timer(t2);
			if (t3) l->cancel_timer(t3);
			if (fd != kInvalidSocket) {
				l->remove(fd);
				close_socket(fd);
			}
			if (conn) conn->close();	//コールバックは weak_ptr なので、もう何も呼ばれない
		});
	}

	//ループのコールバックから、まだ生きていて同じ試行のときだけ Impl を返す
	static Impl* current(const std::weak_ptr<MqttClient>& weak, std::uint64_t g, std::shared_ptr<MqttClient>& hold) {
		hold = weak.lock();
		if (!hold || hold->impl_->gen != g) return nullptr;
		return hold->impl_.get();
	}

	//コールバックは手元に写してから呼ぶ。呼ばれた側が差し替えると、実行中の関数 (と掴んだ値) が壊れるため
	void emit(const std::function<void()> MqttClient::*member) {
		if (auto cb = owner.*member) cb();
	}

	void emit_error(const std::string& message) {
		if (auto cb = owner.on_error) cb(message);
	}

	void cancel(std::uint64_t& timer) {
		if (timer) loop.cancel_timer(timer);
		timer = 0;
	}

	void send(const std::string& data) {
		std::shared_ptr<TcpConn> conn = tcp;
		if (!conn) return;
		last_send = steady_ms();
		conn->write(data);	//失敗するとこの中で閉じて drop まで進む
	}

	void begin_attempt(bool reconnect) {
		if (reconnect) {
			emit(&MqttClient::on_reconnect);
			if (state != State::Waiting) return;	//コールバックの中で end された
		}
		const std::uint64_t g = ++gen;
		state = State::Connecting;
		parser = MqttPacketParser(options.max_packet_bytes);

		//名前解決はループのスレッドで行う (connect_websocket と同じ制約)
		try {
			addrs = resolve_tcp(url.host, url.port);
		} catch (const std::exception& ex) {
			return drop(ex.what());
		}
		addr_index = 0;

		std::weak_ptr<MqttClient> weak = owner.weak_from_this();
		connect_timer = loop.set_timer(options.connect_timeout_ms, [weak, g] {
			std::shared_ptr<MqttClient> hold;
			if (Impl* s = current(weak, g, hold)) {
				s->connect_timer = 0;
				s->drop("connect timeout");
			}
		});
		try_next();
	}

	//候補のアドレスを順に試す
	void try_next() {
		const std::uint64_t g = gen;
		std::weak_ptr<MqttClient> weak = owner.weak_from_this();
		while (addr_index < addrs.size()) {
			bool in_progress = false;
			const socket_t fd = start_connect(addrs[addr_index++], &in_progress);
			if (fd == kInvalidSocket) continue;
			if (!in_progress) return connected(fd);

			pending_fd = fd;
			loop.add(fd, kWrite, [weak, g](unsigned) {
				std::shared_ptr<MqttClient> hold;
				Impl* s = current(weak, g, hold);
				if (!s) return;	//手放された: 後始末はデストラクタが積んだ処理が行う
				const socket_t done_fd = s->pending_fd;
				s->loop.remove(done_fd);
				s->pending_fd = kInvalidSocket;
				if (socket_error(done_fd) == 0) return s->connected(done_fd);
				close_socket(done_fd);
				s->try_next();
			});
			return;
		}
		drop("cannot connect to " + url.host_header());
	}

	void connected(socket_t fd) {
		const std::uint64_t g = gen;
		std::weak_ptr<MqttClient> weak = owner.weak_from_this();
		//接続のコールバックはクライアントを weak で持つ。手放されたクライアントを接続が生かし続けないように
		tcp = std::make_shared<TcpConn>(loop, fd, kMaxBuffered);
		tcp->set_on_data([weak, g](const char* data, std::size_t len) {
			std::shared_ptr<MqttClient> hold;
			if (Impl* s = current(weak, g, hold)) s->on_data(std::string_view(data, len));
		});
		tcp->set_on_close([weak, g] {
			std::shared_ptr<MqttClient> hold;
			if (Impl* s = current(weak, g, hold)) s->drop("");
		});
		tcp->start();
		state = State::WaitConnack;
		last_recv = steady_ms();
		send(connect_packet);
	}

	void on_data(std::string_view chunk) {
		last_recv = steady_ms();
		const std::uint64_t g = gen;
		std::vector<MqttPacket> packets;
		try {
			packets = parser.push(chunk);
		} catch (const MqttProtocolError& ex) {
			return drop(ex.what());
		}
		for (const MqttPacket& packet : packets) {
			try {
				on_packet(packet);
			} catch (const MqttProtocolError& ex) {
				if (gen == g) drop(ex.what());
				return;
			}
			if (gen != g) return;	//コールバックの中で end された / 切れた
			//コールバックの中で手放された (残るのはこの呼び出しが掴んでいる分だけ)。手放した相手に残りを届けない
			if (owner.weak_from_this().use_count() <= 1) return;
		}
	}

	void on_packet(const MqttPacket& packet) {
		if (state == State::WaitConnack) {
			//ブローカーが最初に送るのは CONNACK と決まっている (§3.2)
			if (packet.type != MqttType::Connack) throw MqttProtocolError("expected CONNACK");
			const MqttConnack ack = decode_mqtt_connack(packet);
			if (ack.return_code != 0) {
				return drop("connection refused: " + std::to_string(ack.return_code) + " (" + refusal_reason(ack.return_code) + ")");
			}
			//clean session なので、セッションが残っていると言われたら食い違っている (§3.2.2.2)
			if (ack.session_present) throw MqttProtocolError("unexpected session present");
			cancel(connect_timer);
			state = State::Connected;
			arm_keep_alive();
			emit(&MqttClient::on_connect);
			return;
		}
		switch (packet.type) {
		case MqttType::Publish:
			return on_publish(packet);
		case MqttType::Suback:
			return on_suback(packet);
		case MqttType::Pingresp:
			if (!packet.body.empty()) throw MqttProtocolError("invalid PINGRESP");
			return;
		default:
			throw MqttProtocolError("unexpected packet type " + std::to_string(static_cast<int>(packet.type)));
		}
	}

	void on_publish(const MqttPacket& packet) {
		const MqttPublish pub = decode_mqtt_publish(packet);
		//QoS 2 は購読時に求めないので、届くのはブローカーの誤り
		if (pub.qos == 2) throw MqttProtocolError("QoS 2 PUBLISH is not supported");
		const std::uint64_t g = gen;
		if (auto cb = owner.on_message) cb(pub.topic, pub.payload);
		if (gen != g || state != State::Connected) return;
		//処理を終えてから受領を返す
		if (pub.qos == 1) send(encode_mqtt_puback(pub.packet_id));
	}

	void on_suback(const MqttPacket& packet) {
		const MqttSuback ack = decode_mqtt_suback(packet);
		auto it = pending_subs.find(ack.packet_id);
		if (it == pending_subs.end()) throw MqttProtocolError("unexpected SUBACK");
		if (ack.return_codes.size() != 1) throw MqttProtocolError("SUBACK return code count mismatch");	//1 回に 1 つだけ購読する
		std::function<void(bool)> done = std::move(it->second);
		pending_subs.erase(it);
		if (done) done(ack.return_codes[0] != 0x80);
	}

	//何も送らずに keep-alive が過ぎたら PINGREQ、1.5 倍の間何も届かなければ切れたとみなす (§3.1.2.10)
	void arm_keep_alive() {
		if (keep_alive_ms <= 0) return;
		const std::int64_t ping_at = last_send + keep_alive_ms;
		const std::int64_t dead_at = last_recv + keep_alive_ms * 3 / 2;
		std::int64_t delay = (ping_at < dead_at ? ping_at : dead_at) - steady_ms();
		if (delay < 1) delay = 1;

		const std::uint64_t g = gen;
		std::weak_ptr<MqttClient> weak = owner.weak_from_this();
		keep_timer = loop.set_timer(static_cast<int>(delay), [weak, g] {
			std::shared_ptr<MqttClient> hold;
			if (Impl* s = current(weak, g, hold)) {
				s->keep_timer = 0;
				s->on_keep_alive();
			}
		});
	}

	void on_keep_alive() {
		const std::int64_t now = steady_ms();
		if (now - last_recv >= keep_alive_ms * 3 / 2) return drop("keep-alive timeout");
		const std::uint64_t g = gen;
		if (now - last_send >= keep_alive_ms) send(encode_mqtt_pingreq());
		if (gen == g && state == State::Connected) arm_keep_alive();
	}

	std::uint16_t allocate_packet_id() {
		for (int i = 0; i < 65535; ++i) {
			if (++next_packet_id == 0) next_packet_id = 1;
			if (!pending_subs.count(next_packet_id)) return next_packet_id;
		}
		return 0;
	}

	void fail_subscriptions() {
		std::map<std::uint16_t, std::function<void(bool)>> pending;
		pending.swap(pending_subs);
		for (auto& kv : pending) {
			if (kv.second) kv.second(false);
		}
	}

	//接続 (または試行) を捨て、再接続を予約する。error が空なら相手が閉じただけ
	void drop(const std::string& error) {
		if (state != State::Connecting && state != State::WaitConnack && state != State::Connected) return;
		++gen;
		state = State::Waiting;
		cancel(connect_timer);
		cancel(keep_timer);
		if (pending_fd != kInvalidSocket) {
			loop.remove(pending_fd);
			close_socket(pending_fd);
			pending_fd = kInvalidSocket;
		}
		std::shared_ptr<TcpConn> conn = std::move(tcp);
		if (conn) conn->close();	//gen を進めたので、この接続の on_close はもう何もしない

		fail_subscriptions();
		if (!error.empty()) emit_error(error);
		emit(&MqttClient::on_close);
		if (state != State::Waiting) return;	//コールバックの中で end された

		//失敗が続いても間を空ける (すぐ繋ぎ直して空回りしない)
		std::weak_ptr<MqttClient> weak = owner.weak_from_this();
		reconnect_timer = loop.set_timer(options.reconnect_ms, [weak] {
			std::shared_ptr<MqttClient> self = weak.lock();
			if (!self) return;
			Impl& s = *self->impl_;
			s.reconnect_timer = 0;
			if (s.state == State::Waiting) s.begin_attempt(true);
		});
	}

	void finish() {
		if (state == State::Ended) return;
		const State was = state;
		state = State::Ended;
		++gen;
		cancel(connect_timer);
		cancel(keep_timer);
		cancel(reconnect_timer);
		if (pending_fd != kInvalidSocket) {
			loop.remove(pending_fd);
			close_socket(pending_fd);
			pending_fd = kInvalidSocket;
		}
		std::shared_ptr<TcpConn> conn = std::move(tcp);
		if (conn && was == State::Connected) {
			conn->write(encode_mqtt_disconnect());
			conn->end();	//送り切ってから閉じる。ブローカーが閉じなくても時間で切る
			std::weak_ptr<TcpConn> weak_conn = conn;
			loop.set_timer(kDisconnectWaitMs, [weak_conn] {
				if (auto c = weak_conn.lock()) c->close();
			});
		} else if (conn) {
			conn->close();
		}

		fail_subscriptions();
		if (was == State::Connecting || was == State::WaitConnack || was == State::Connected) emit(&MqttClient::on_close);

		//コールバックがクライアント自身を掴んでいても解放されるよう、後で外す (実行中のコールバックを壊さないため今は外さない)
		std::shared_ptr<MqttClient> self = owner.shared_from_this();
		loop.post([self] {
			self->on_connect = nullptr;
			self->on_message = nullptr;
			self->on_reconnect = nullptr;
			self->on_close = nullptr;
			self->on_error = nullptr;
		});
	}
};

namespace {

//URL と設定を検証し、送る CONNECT を組み立てる。不正なら std::invalid_argument
std::string validate(const MqttOptions& options, Url& url) {
	const std::size_t sep = options.url.find("://");
	const std::string scheme = to_lower(options.url.substr(0, sep == std::string::npos ? 0 : sep));
	if (scheme == "mqtts") throw std::invalid_argument("mqtts:// is not supported (terminate TLS at a reverse proxy)");
	if (scheme != "mqtt") throw std::invalid_argument("unsupported protocol " + scheme);
	if (!parse_url(options.url, url)) throw std::invalid_argument("invalid url");
	//黙って無視しない。認証情報は username / password で渡す
	if (!url.userinfo.empty()) throw std::invalid_argument("credentials in mqtt url are not supported (use username / password)");
	if (url.target != "/") throw std::invalid_argument("mqtt url must not have a path");

	if (!utf8_valid(options.client_id) || !utf8_valid(options.username)) throw std::invalid_argument("client_id and username must be utf-8");
	if (options.reconnect_ms <= 0) throw std::invalid_argument("reconnect_ms must be positive");
	if (options.connect_timeout_ms <= 0) throw std::invalid_argument("connect_timeout_ms must be positive");
	if (options.max_packet_bytes < 4) throw std::invalid_argument("max_packet_bytes too small");
	//長さ・keep-alive の範囲・username 無しの password は組み立てで弾く
	return encode_mqtt_connect(options.client_id, options.username, options.password, options.keep_alive_sec);
}

}

MqttClient::MqttClient(EventLoop& loop, MqttOptions options) {
	Url url;
	std::string connect_packet = validate(options, url);
	impl_.reset(new Impl(*this, loop, std::move(options), std::move(url), std::move(connect_packet)));
}

MqttClient::~MqttClient() = default;

std::shared_ptr<MqttClient> MqttClient::create(EventLoop& loop, MqttOptions options) {
	return std::shared_ptr<MqttClient>(new MqttClient(loop, std::move(options)));
}

void MqttClient::start() {
	//必ず後で始める。呼び出し側がコールバックを設定し終えてから、ループスレッドで動かす
	std::shared_ptr<MqttClient> self = shared_from_this();
	impl_->loop.post([self] {
		Impl& s = *self->impl_;
		if (s.state == State::Idle) s.begin_attempt(false);
	});
}

void MqttClient::subscribe(std::string topic_filter, int qos, std::function<void(bool ok)> done) {
	if (qos != 0 && qos != 1) throw std::invalid_argument("qos must be 0 or 1");
	if (topic_filter.empty() || topic_filter.size() > 65535 || !utf8_valid(topic_filter)) throw std::invalid_argument("invalid topic filter");

	std::shared_ptr<MqttClient> self = shared_from_this();
	impl_->loop.dispatch([self, topic = std::move(topic_filter), qos, done = std::move(done)]() mutable {
		Impl& s = *self->impl_;
		if (s.state != State::Connected) {
			if (done) done(false);
			return;
		}
		const std::uint16_t id = s.allocate_packet_id();
		if (id == 0) {
			if (done) done(false);
			return;
		}
		s.pending_subs[id] = std::move(done);	//done が空でも、SUBACK の照合に使う
		s.send(encode_mqtt_subscribe(id, topic, qos));
	});
}

void MqttClient::end() {
	std::shared_ptr<MqttClient> self = shared_from_this();
	impl_->loop.dispatch([self] { self->impl_->finish(); });
}

void mqtt_set_keep_alive_ms_for_test(MqttClient& client, int ms) {
	client.impl_->keep_alive_ms = ms;
}

}
