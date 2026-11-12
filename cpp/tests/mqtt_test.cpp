#include <condition_variable>
#include <future>

#include "portus/mqtt.hpp"
#include "portus/socket.hpp"
#include "test.hpp"

using namespace portus;

namespace {

using Clock = std::chrono::steady_clock;

std::string bytes(std::initializer_list<int> list) {
	std::string out;
	for (int b : list) out.push_back(static_cast<char>(b));
	return out;
}

//仕様 (3.1.1 §1.5.3) の 2 バイト長 + UTF-8 文字列
std::string mqtt_str(const std::string& s) {
	return bytes({ static_cast<int>(s.size() >> 8), static_cast<int>(s.size() & 0xFF) }) + s;
}

std::string publish_packet(const std::string& topic, const std::string& payload, int qos, int packet_id = 0) {
	std::string body = mqtt_str(topic);
	if (qos > 0) body += bytes({ packet_id >> 8, packet_id & 0xFF });
	body += payload;
	return std::string(1, static_cast<char>(0x30 | (qos << 1))) + encode_mqtt_remaining_length(body.size()) + body;
}

std::vector<MqttPacket> parse_all(const std::string& data, std::size_t limit = kMqttDefaultMaxPacket) {
	return MqttPacketParser(limit).push(data);
}

void send_all(socket_t fd, const std::string& data) {
	std::size_t sent = 0;
	while (sent < data.size()) {
		const long n = socket_send(fd, data.data() + sent, data.size() - sent);
		if (n > 0) sent += static_cast<std::size_t>(n);
		else if (!last_error_would_block()) return;
		else wait_socket(fd, kWrite, 50);
	}
}

struct Seen {
	int conn;
	MqttPacket packet;
};

//最小のブローカーもどき。接続を 1 本ずつ受け、受けたパケットを記録して決まった応答を返す
struct FakeBroker {
	Listener listener;
	std::thread thread;
	std::atomic<bool> stop{false};

	std::atomic<bool> silent{false};		//何も返さない (接続は受ける)
	std::atomic<bool> answer_ping{true};
	std::atomic<int> refuse_first{0};		//最初の n 接続は CONNACK で断る
	std::atomic<int> refuse_code{5};
	std::atomic<int> suback_code{-1};		//-1 なら求められた QoS を許可する
	std::atomic<int> connections{0};
	std::atomic<int> closed{0};				//相手が閉じた接続の数

	std::mutex mu;
	std::vector<Seen> seen;
	std::vector<std::string> outbox;
	std::vector<Clock::time_point> accepted_at;
	bool drop_request = false;

	FakeBroker() : listener(listen_tcp("127.0.0.1", 0)) {
		thread = std::thread([this] { serve(); });
	}
	~FakeBroker() {
		stop = true;
		thread.join();
		close_socket(listener.fd);
	}

	std::string url() const { return "mqtt://127.0.0.1:" + std::to_string(listener.port); }

	void send(const std::string& data) {
		std::lock_guard<std::mutex> lock(mu);
		outbox.push_back(data);
	}
	void drop() {
		std::lock_guard<std::mutex> lock(mu);
		drop_request = true;
	}
	std::vector<Seen> packets(MqttType type) {
		std::lock_guard<std::mutex> lock(mu);
		std::vector<Seen> out;
		for (const Seen& s : seen) {
			if (s.packet.type == type) out.push_back(s);
		}
		return out;
	}
	int count(MqttType type) { return static_cast<int>(packets(type).size()); }

	std::string respond(int conn, const MqttPacket& p) {
		if (silent) return "";
		switch (p.type) {
		case MqttType::Connect:
			return bytes({ 0x20, 0x02, 0x00, conn <= refuse_first ? refuse_code.load() : 0 });
		case MqttType::Subscribe: {
			const int code = suback_code >= 0 ? suback_code.load() : static_cast<unsigned char>(p.body.back());
			return bytes({ 0x90, 0x03, static_cast<unsigned char>(p.body[0]), static_cast<unsigned char>(p.body[1]), code });
		}
		case MqttType::Pingreq:
			return answer_ping ? bytes({ 0xD0, 0x00 }) : "";
		default:
			return "";
		}
	}

	void serve() {
		socket_t fd = kInvalidSocket;
		MqttPacketParser parser;
		int conn = 0;
		char buf[65536];
		while (!stop) {
			if (fd == kInvalidSocket) {
				if (!wait_socket(listener.fd, kRead, 10)) continue;
				fd = accept_tcp(listener.fd, nullptr);
				if (fd == kInvalidSocket) continue;
				conn = ++connections;
				parser = MqttPacketParser();
				std::lock_guard<std::mutex> lock(mu);
				accepted_at.push_back(Clock::now());
				continue;
			}
			std::vector<std::string> out;
			{
				std::lock_guard<std::mutex> lock(mu);
				if (drop_request) {
					drop_request = false;
					close_socket(fd);
					fd = kInvalidSocket;
					continue;
				}
				out.swap(outbox);
			}
			for (const std::string& o : out) send_all(fd, o);
			if (!wait_socket(fd, kRead, 10)) continue;
			const long n = socket_recv(fd, buf, sizeof(buf));
			if (n < 0 && last_error_would_block()) continue;
			if (n <= 0) {
				close_socket(fd);
				fd = kInvalidSocket;
				++closed;
				continue;
			}
			for (MqttPacket& p : parser.push(std::string_view(buf, static_cast<std::size_t>(n)))) {
				const std::string reply = respond(conn, p);
				{
					std::lock_guard<std::mutex> lock(mu);
					seen.push_back(Seen{ conn, std::move(p) });
				}
				if (!reply.empty()) send_all(fd, reply);
			}
		}
		if (fd != kInvalidSocket) close_socket(fd);
	}
};

//コールバックの呼ばれた順を記録する
struct Recorder {
	std::mutex mu;
	std::vector<std::string> events;

	void add(const std::string& e) {
		std::lock_guard<std::mutex> lock(mu);
		events.push_back(e);
	}
	std::vector<std::string> list() {
		std::lock_guard<std::mutex> lock(mu);
		return events;
	}
	int count(const std::string& prefix) {
		int n = 0;
		for (const std::string& e : list()) {
			if (e.compare(0, prefix.size(), prefix) == 0) ++n;
		}
		return n;
	}
	bool wait(const std::string& prefix, int n = 1, int timeout_ms = 3000) {
		return testing::wait_until([&] { return count(prefix) >= n; }, timeout_ms);
	}
};

MqttOptions options_for(const FakeBroker& broker) {
	MqttOptions o;
	o.url = broker.url();
	o.client_id = "test";
	o.reconnect_ms = 100;
	o.connect_timeout_ms = 2000;
	return o;
}

//end はループへ渡されて後で動く。終わるまで待ってから Recorder などを片付ける
void end_and_wait(EventLoop& loop, const std::shared_ptr<MqttClient>& client) {
	client->end();
	loop.dispatch_sync([] {});
}

//OwnTracks の購読側と同じ使い方: 接続のたびに QoS 1 で購読し直す
std::shared_ptr<MqttClient> make_client(EventLoop& loop, MqttOptions options, Recorder& rec, bool subscribe = true) {
	std::shared_ptr<MqttClient> client = MqttClient::create(loop, std::move(options));
	std::weak_ptr<MqttClient> weak = client;
	client->on_connect = [weak, &rec, subscribe] {
		rec.add("connect");
		auto c = weak.lock();
		if (!c || !subscribe) return;
		c->subscribe("owntracks/+/+", 1, [&rec](bool ok) { rec.add(ok ? "suback:ok" : "suback:fail"); });
	};
	client->on_message = [&rec](std::string_view topic, std::string_view payload) { rec.add("msg:" + std::string(topic) + "=" + std::string(payload)); };
	client->on_reconnect = [&rec] { rec.add("reconnect"); };
	client->on_close = [&rec] { rec.add("close"); };
	client->on_error = [&rec](const std::string& m) { rec.add("error:" + m); };
	return client;
}

}

TEST(mqtt_remaining_length, "MQTT: 残り長さの可変長整数は仕様の表 (§2.2.3) と一致する") {
	CHECK_EQ(encode_mqtt_remaining_length(0), bytes({ 0x00 }));
	CHECK_EQ(encode_mqtt_remaining_length(127), bytes({ 0x7F }));
	CHECK_EQ(encode_mqtt_remaining_length(128), bytes({ 0x80, 0x01 }));
	CHECK_EQ(encode_mqtt_remaining_length(16383), bytes({ 0xFF, 0x7F }));
	CHECK_EQ(encode_mqtt_remaining_length(16384), bytes({ 0x80, 0x80, 0x01 }));
	CHECK_EQ(encode_mqtt_remaining_length(2097151), bytes({ 0xFF, 0xFF, 0x7F }));
	CHECK_EQ(encode_mqtt_remaining_length(2097152), bytes({ 0x80, 0x80, 0x80, 0x01 }));
	CHECK_EQ(encode_mqtt_remaining_length(268435455), bytes({ 0xFF, 0xFF, 0xFF, 0x7F }));
	CHECK_THROWS(encode_mqtt_remaining_length(268435456));
}

TEST(mqtt_encode, "MQTT: CONNECT (認証あり / なし)・SUBSCRIBE・PUBACK・PINGREQ・DISCONNECT のバイト列") {
	//§3.1: 固定ヘッダ 0x10、"MQTT"、レベル 4、フラグ (clean session = 0x02)、keep-alive、client id
	CHECK_EQ(encode_mqtt_connect("abc", "", "", 60),
		bytes({ 0x10, 0x0F, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C, 0x00, 0x03, 'a', 'b', 'c' }));
	//username (0x80) / password (0x40) のフラグと、client id の後に username → password の順
	CHECK_EQ(encode_mqtt_connect("abc", "u", "pw", 60),
		bytes({ 0x10, 0x16, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xC2, 0x00, 0x3C, 0x00, 0x03, 'a', 'b', 'c', 0x00, 0x01, 'u', 0x00, 0x02, 'p', 'w' }));
	CHECK_EQ(encode_mqtt_connect("", "u", "", 0), bytes({ 0x10, 0x0F, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 'u' }));
	CHECK_THROWS(encode_mqtt_connect("abc", "", "pw", 60));
	CHECK_THROWS(encode_mqtt_connect("abc", "", "", 65536));
	CHECK_THROWS(encode_mqtt_connect(std::string(65536, 'a'), "", "", 60));

	//§3.8: 固定ヘッダ 0x82 (予約フラグ 0010)、packet id、トピックフィルタ、要求 QoS
	CHECK_EQ(encode_mqtt_subscribe(10, "a/b", 1), bytes({ 0x82, 0x08, 0x00, 0x0A, 0x00, 0x03, 'a', '/', 'b', 0x01 }));
	CHECK_THROWS(encode_mqtt_subscribe(10, "a/b", 2));
	CHECK_THROWS(encode_mqtt_subscribe(0, "a/b", 1));

	CHECK_EQ(encode_mqtt_puback(0x1234), bytes({ 0x40, 0x02, 0x12, 0x34 }));
	CHECK_EQ(encode_mqtt_pingreq(), bytes({ 0xC0, 0x00 }));
	CHECK_EQ(encode_mqtt_disconnect(), bytes({ 0xE0, 0x00 }));
}

TEST(mqtt_parse_split, "MQTT: 残り長さの境目 (127/128/16383/16384) と、1 バイトずつ届いた場合の解析") {
	std::string stream;
	const std::size_t sizes[] = { 0, 127, 128, 16383, 16384 };
	for (std::size_t n : sizes) stream += std::string(1, '\x30') + encode_mqtt_remaining_length(n) + std::string(n, 'x');
	CHECK_EQ(stream.substr(1 + 1 + 0, 2), bytes({ 0x30, 0x7F }));
	stream += bytes({ 0x20, 0x02, 0x00, 0x00, 0x90, 0x03, 0x00, 0x01, 0x01, 0xD0, 0x00 });

	const std::vector<MqttPacket> whole = parse_all(stream);
	CHECK_EQ(whole.size(), 8u);
	for (std::size_t i = 0; i < 5 && i < whole.size(); ++i) {
		CHECK(whole[i].type == MqttType::Publish);
		CHECK_EQ(whole[i].body.size(), sizes[i]);
	}

	MqttPacketParser parser;
	std::vector<MqttPacket> got;
	for (char c : stream) {
		for (MqttPacket& p : parser.push(std::string_view(&c, 1))) got.push_back(std::move(p));
	}
	CHECK_EQ(got.size(), 8u);
	if (got.size() == 8u) {
		CHECK_EQ(got[4].body.size(), 16384u);
		CHECK(got[5].type == MqttType::Connack);
		CHECK(got[6].type == MqttType::Suback);
		CHECK_EQ(got[6].flags, 0u);
		CHECK(got[7].type == MqttType::Pingresp);
		CHECK(got[7].body.empty());
	}
}

TEST(mqtt_parse_reject, "MQTT: 上限超過は本体を待たずに、5 バイトの残り長さ・不正な種別やフラグは拒否する") {
	//固定ヘッダ (3 バイト) だけで、200 バイトの本体を待たずに上限 100 を超えると分かる
	CHECK_THROWS(MqttPacketParser(100).push(bytes({ 0x30, 0xC8, 0x01 })));
	//上限は固定ヘッダ込み。ちょうどは通り、1 バイト超えると拒否
	CHECK_EQ(parse_all(std::string(1, '\x30') + encode_mqtt_remaining_length(98) + std::string(98, 'x'), 100).size(), 1u);
	CHECK_THROWS(parse_all(std::string(1, '\x30') + encode_mqtt_remaining_length(99) + std::string(99, 'x'), 100));
	//4 バイト目にも継続ビットが立っていれば、5 バイト目を待たずに拒否
	CHECK_THROWS(parse_all(bytes({ 0x30, 0xFF, 0xFF, 0xFF, 0xFF })));
	CHECK_THROWS(parse_all(bytes({ 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 })));
	//途中までなら待つ
	CHECK(parse_all(bytes({ 0x30, 0xFF, 0xFF, 0xFF })).empty());

	CHECK_THROWS(parse_all(bytes({ 0x00, 0x00 })));	//種別 0 は予約
	CHECK_THROWS(parse_all(bytes({ 0xF0, 0x00 })));	//種別 15 は予約
	CHECK_THROWS(parse_all(bytes({ 0x21, 0x02, 0x00, 0x00 })));	//CONNACK のフラグは 0
	CHECK_THROWS(parse_all(bytes({ 0x80, 0x00 })));	//SUBSCRIBE のフラグは 0010
	CHECK_THROWS(parse_all(bytes({ 0x36, 0x00 })));	//PUBLISH の QoS 3
}

TEST(mqtt_decode, "MQTT: PUBLISH / CONNACK / SUBACK の中身の検証") {
	std::vector<MqttPacket> p = parse_all(publish_packet("owntracks/u/d", "{\"a\":1}", 1, 0x1234));
	CHECK_EQ(p.size(), 1u);
	const MqttPublish pub = decode_mqtt_publish(p[0]);
	CHECK_EQ(pub.topic, "owntracks/u/d");
	CHECK_EQ(pub.payload, "{\"a\":1}");
	CHECK_EQ(pub.qos, 1);
	CHECK_EQ(pub.packet_id, 0x1234);

	const MqttPublish q0 = decode_mqtt_publish(parse_all(publish_packet("t", "", 0))[0]);
	CHECK_EQ(q0.qos, 0);
	CHECK_EQ(q0.payload, "");

	const auto bad = [](const std::string& raw) { return decode_mqtt_publish(parse_all(raw)[0]); };
	CHECK_THROWS(bad(publish_packet("a/\xC3", "x", 0)));		//不正な UTF-8
	CHECK_THROWS(bad(publish_packet("a/+", "x", 0)));			//トピック名にワイルドカード
	CHECK_THROWS(bad(publish_packet("a/#", "x", 0)));
	CHECK_THROWS(bad(publish_packet(std::string("a\0b", 3), "x", 0)));	//U+0000
	CHECK_THROWS(bad(publish_packet("", "x", 0)));				//空のトピック
	CHECK_THROWS(bad(publish_packet("t", "", 1, 0)));			//packet id 0
	CHECK_THROWS(bad(bytes({ 0x38, 0x03, 0x00, 0x01, 't' })));	//QoS 0 に DUP
	CHECK_THROWS(bad(bytes({ 0x30, 0x03, 0x00, 0x05, 't' })));	//トピック長が本体を超える
	CHECK_THROWS(bad(bytes({ 0x32, 0x04, 0x00, 0x01, 't', 0x00 })));	//packet id が切れている

	CHECK_EQ(decode_mqtt_connack(parse_all(bytes({ 0x20, 0x02, 0x01, 0x05 }))[0]).return_code, 5);
	CHECK(decode_mqtt_connack(parse_all(bytes({ 0x20, 0x02, 0x01, 0x00 }))[0]).session_present);
	CHECK_THROWS(decode_mqtt_connack(parse_all(bytes({ 0x20, 0x02, 0x02, 0x00 }))[0]));
	CHECK_THROWS(decode_mqtt_connack(parse_all(bytes({ 0x20, 0x03, 0x00, 0x00, 0x00 }))[0]));

	const MqttSuback sa = decode_mqtt_suback(parse_all(bytes({ 0x90, 0x04, 0x00, 0x07, 0x01, 0x80 }))[0]);
	CHECK_EQ(sa.packet_id, 7);
	CHECK(sa.return_codes == std::vector<int>({ 0x01, 0x80 }));
	CHECK_THROWS(decode_mqtt_suback(parse_all(bytes({ 0x90, 0x03, 0x00, 0x07, 0x03 }))[0]));
	CHECK_THROWS(decode_mqtt_suback(parse_all(bytes({ 0x90, 0x02, 0x00, 0x07 }))[0]));
}

TEST(mqtt_options, "MQTT: mqtts・他のスキーム・URL の認証情報やパス・username 無しの password は生成時に拒否する") {
	LoopThread lt;
	const auto make = [&](const std::string& url, const std::string& user = "", const std::string& pass = "") {
		MqttOptions o;
		o.url = url;
		o.username = user;
		o.password = pass;
		return MqttClient::create(lt.loop(), o);
	};
	CHECK(make("mqtt://localhost") != nullptr);	//既定ポート 1883
	CHECK(make("MQTT://127.0.0.1:1884", "u", "p") != nullptr);
	CHECK_THROWS(make("mqtts://localhost"));
	CHECK_THROWS(make("ws://localhost"));
	CHECK_THROWS(make("localhost:1883"));
	CHECK_THROWS(make("mqtt://"));
	CHECK_THROWS(make("mqtt://u:p@localhost"));
	CHECK_THROWS(make("mqtt://localhost/x"));
	CHECK_THROWS(make("mqtt://localhost", "", "p"));
	CHECK_THROWS(make("mqtt://localhost", "\xFF"));
}

TEST(mqtt_subscribe_deliver, "MQTT: 接続 → 購読 → QoS 0 / 1 の受信。QoS 1 は処理後に同じ packet id で PUBACK、認証情報も送る") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	MqttOptions o = options_for(broker);
	o.client_id = "cid";
	o.username = "user";
	o.password = "secret";
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);

	//未接続の subscribe は false
	std::promise<bool> early;
	client->subscribe("a", 0, [&](bool ok) { early.set_value(ok); });
	CHECK(!early.get_future().get());
	CHECK_THROWS(client->subscribe("a", 2));
	CHECK_THROWS(client->subscribe("", 1));

	std::atomic<int> pubacks_during_callback{-1};
	client->on_message = [&](std::string_view topic, std::string_view payload) {
		if (payload == "q1") {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			pubacks_during_callback = broker.count(MqttType::Puback);
		}
		rec.add("msg:" + std::string(topic) + "=" + std::string(payload));
	};
	client->start();
	CHECK(rec.wait("suback:ok"));

	//CONNECT の本体: "MQTT" 4、フラグ 0xC2 (username + password + clean session)、keep-alive 60、client id、username、password
	const std::vector<Seen> connects = broker.packets(MqttType::Connect);
	CHECK_EQ(connects.size(), 1u);
	if (!connects.empty()) {
		CHECK_EQ(connects[0].packet.body, mqtt_str("MQTT") + bytes({ 0x04, 0xC2, 0x00, 0x3C }) + mqtt_str("cid") + mqtt_str("user") + mqtt_str("secret"));
	}
	const std::vector<Seen> subs = broker.packets(MqttType::Subscribe);
	CHECK_EQ(subs.size(), 1u);
	if (!subs.empty()) {
		const std::string& b = subs[0].packet.body;
		CHECK_EQ(subs[0].packet.flags, 2u);
		CHECK(b.size() > 2 && (b[0] != 0 || b[1] != 0));	//packet id は 0 以外
		CHECK_EQ(b.substr(2), mqtt_str("owntracks/+/+") + bytes({ 0x01 }));
	}

	broker.send(publish_packet("owntracks/me/phone", "q0", 0) + publish_packet("owntracks/me/phone", "q1", 1, 0x1234));
	CHECK(rec.wait("msg:owntracks/me/phone=q1"));
	CHECK(testing::wait_until([&] { return broker.count(MqttType::Puback) == 1; }));
	const std::vector<Seen> acks = broker.packets(MqttType::Puback);
	if (!acks.empty()) CHECK_EQ(acks[0].packet.body, bytes({ 0x12, 0x34 }));
	CHECK_EQ(pubacks_during_callback.load(), 0);	//on_message が戻るまで PUBACK は送らない
	CHECK_EQ(rec.count("msg:owntracks/me/phone=q0"), 1);

	//ブローカーが購読を断る (0x80) と false
	broker.suback_code = 0x80;
	std::promise<bool> refused;
	client->subscribe("x/y", 1, [&](bool ok) { refused.set_value(ok); });
	CHECK(!refused.get_future().get());

	end_and_wait(lt.loop(), client);
	CHECK(rec.wait("close"));
	CHECK_EQ(rec.count("error"), 0);
	CHECK_EQ(broker.connections.load(), 1);
}

TEST(mqtt_refused_retry, "MQTT: CONNACK で断られたら on_error → on_close、reconnect_ms 空けて繋ぎ直す") {
	LoopThread lt;
	FakeBroker broker;
	broker.refuse_first = 1;
	broker.refuse_code = 5;
	Recorder rec;
	MqttOptions o = options_for(broker);
	o.reconnect_ms = 150;
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);
	client->start();

	CHECK(rec.wait("suback:ok"));
	const std::vector<std::string> ev = rec.list();
	CHECK(ev.size() >= 5);
	if (ev.size() >= 5) {
		CHECK_EQ(ev[0], "error:connection refused: 5 (not authorized)");
		CHECK_EQ(ev[1], "close");
		CHECK_EQ(ev[2], "reconnect");
		CHECK_EQ(ev[3], "connect");
	}
	{
		std::lock_guard<std::mutex> lock(broker.mu);
		CHECK_EQ(broker.accepted_at.size(), 2u);
		if (broker.accepted_at.size() >= 2) {
			const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(broker.accepted_at[1] - broker.accepted_at[0]).count();
			CHECK(gap >= 140);
		}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK_EQ(broker.connections.load(), 2);	//受け入れられた後は繋ぎ直さない
	end_and_wait(lt.loop(), client);
}

TEST(mqtt_broker_drop, "MQTT: ブローカーが切ったら on_close → on_reconnect → on_connect、購読し直しが届く") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), options_for(broker), rec);
	client->start();
	CHECK(rec.wait("suback:ok"));

	broker.drop();
	CHECK(rec.wait("suback:ok", 2));
	const std::vector<std::string> ev = rec.list();
	CHECK(ev == std::vector<std::string>({ "connect", "suback:ok", "close", "reconnect", "connect", "suback:ok" }));
	const std::vector<Seen> subs = broker.packets(MqttType::Subscribe);
	CHECK_EQ(subs.size(), 2u);
	if (subs.size() == 2u) CHECK_EQ(subs[1].conn, 2);

	broker.send(publish_packet("owntracks/a/b", "after", 0));
	CHECK(rec.wait("msg:owntracks/a/b=after"));
	end_and_wait(lt.loop(), client);
}

TEST(mqtt_keep_alive, "MQTT: 送信が途絶えたら PINGREQ、1.5 倍の間何も届かなければ切って繋ぎ直す") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	MqttOptions o = options_for(broker);
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec, false);
	mqtt_set_keep_alive_ms_for_test(*client, 100);
	client->start();
	CHECK(rec.wait("connect"));

	CHECK(testing::wait_until([&] { return broker.count(MqttType::Pingreq) >= 3; }));
	CHECK_EQ(broker.connections.load(), 1);
	CHECK_EQ(rec.count("close"), 0);

	broker.answer_ping = false;
	CHECK(rec.wait("error:keep-alive timeout"));
	CHECK(rec.wait("connect", 2));
	CHECK_EQ(broker.connections.load(), 2);
	end_and_wait(lt.loop(), client);
}

TEST(mqtt_connect_timeout, "MQTT: 接続を受けて何も返さないブローカーは connect_timeout_ms で諦め、繋がらない先は on_error") {
	LoopThread lt;
	{
		FakeBroker broker;
		broker.silent = true;
		Recorder rec;
		MqttOptions o = options_for(broker);
		o.connect_timeout_ms = 200;
		o.reconnect_ms = 100;
		std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);
		const auto started = Clock::now();
		client->start();
		CHECK(rec.wait("error:connect timeout"));
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
		CHECK(ms >= 190);
		CHECK(rec.wait("reconnect"));
		CHECK(rec.wait("close"));
		CHECK_EQ(rec.count("connect"), 0);
		end_and_wait(lt.loop(), client);
	}
	{
		const Listener l = listen_tcp("127.0.0.1", 0);
		close_socket(l.fd);
		Recorder rec;
		MqttOptions o;
		o.url = "mqtt://127.0.0.1:" + std::to_string(l.port);
		o.reconnect_ms = 100;
		std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);
		client->start();
		CHECK(rec.wait("error:cannot connect"));
		CHECK(rec.wait("close"));
		CHECK(rec.wait("reconnect"));
		end_and_wait(lt.loop(), client);
	}
}

TEST(mqtt_protocol_errors, "MQTT: 上限超過・QoS 2・不正な UTF-8 のトピックは接続を捨てて繋ぎ直し、クライアントは使い続けられる") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	MqttOptions o = options_for(broker);
	o.max_packet_bytes = 1024;
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);
	client->start();
	CHECK(rec.wait("suback:ok"));

	broker.send(publish_packet("owntracks/a/b", std::string(2000, 'x'), 0));
	CHECK(rec.wait("error:packet too large"));
	CHECK(rec.wait("suback:ok", 2));

	broker.send(publish_packet("owntracks/a/b", "two", 2, 1));
	CHECK(rec.wait("error:QoS 2"));
	CHECK(rec.wait("suback:ok", 3));

	broker.send(publish_packet("owntracks/\xFF/b", "bad", 0));
	CHECK(rec.wait("error:invalid PUBLISH topic"));
	CHECK(rec.wait("suback:ok", 4));

	broker.send(publish_packet("owntracks/a/b", "ok", 1, 9));
	CHECK(rec.wait("msg:owntracks/a/b=ok"));
	CHECK_EQ(rec.count("msg:"), 1);
	CHECK_EQ(broker.connections.load(), 4);
	end_and_wait(lt.loop(), client);
}

TEST(mqtt_end, "MQTT: end は DISCONNECT を送り、以後は繋ぎ直さず、on_close は 1 回だけ") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	MqttOptions o = options_for(broker);
	o.reconnect_ms = 50;
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), o, rec);
	client->start();
	CHECK(rec.wait("suback:ok"));

	end_and_wait(lt.loop(), client);
	CHECK(testing::wait_until([&] { return broker.count(MqttType::Disconnect) == 1; }));
	CHECK(testing::wait_until([&] { return broker.closed.load() == 1; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK_EQ(broker.connections.load(), 1);
	CHECK_EQ(rec.count("close"), 1);
	CHECK_EQ(rec.count("reconnect"), 0);
	end_and_wait(lt.loop(), client);	//2 回目は何もしない
	client->start();	//終えた後は始まらない
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK_EQ(broker.connections.load(), 1);
	CHECK_EQ(rec.count("close"), 1);

	//再接続待ちの間の end では on_close は増えない (切れたときにもう呼んでいる)
	Recorder rec2;
	MqttOptions o2 = options_for(broker);
	o2.reconnect_ms = 60000;
	std::shared_ptr<MqttClient> waiting = make_client(lt.loop(), o2, rec2);
	waiting->start();
	CHECK(rec2.wait("suback:ok"));
	broker.drop();
	CHECK(rec2.wait("close"));
	end_and_wait(lt.loop(), waiting);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK_EQ(rec2.count("close"), 1);
}

TEST(mqtt_callback_reentry, "MQTT: on_message の中での差し替え・end・手放しが安全で、自分を掴んだコールバックも解放される") {
	LoopThread lt;
	FakeBroker broker;
	Recorder rec;
	std::shared_ptr<MqttClient> client = make_client(lt.loop(), options_for(broker), rec);
	std::weak_ptr<MqttClient> weak = client;

	//自分自身を強く掴む (循環)。1 通目で自分を差し替えてから掴んだ値を使い、2 通目で end する
	const std::string tag(1000, 'a');
	client->on_message = [client, tag, &rec](std::string_view, std::string_view) {
		client->on_message = [client, &rec](std::string_view, std::string_view payload) {
			rec.add("second:" + std::string(payload));
			client->end();
			client->subscribe("x", 0, [&rec](bool ok) { rec.add(ok ? "late:ok" : "late:fail"); });
		};
		rec.add("first:" + std::to_string(tag.size()));	//差し替えた後も tag は生きている
	};
	client->start();
	CHECK(rec.wait("suback:ok"));
	broker.send(publish_packet("owntracks/a/b", "1", 1, 1) + publish_packet("owntracks/a/b", "2", 1, 2) + publish_packet("owntracks/a/b", "3", 1, 3));
	CHECK(rec.wait("late:fail"));
	CHECK_EQ(rec.count("first:1000"), 1);
	CHECK_EQ(rec.count("second:2"), 1);
	CHECK_EQ(rec.count("second:3"), 0);	//end の後は届けない
	CHECK_EQ(rec.count("close"), 1);
	CHECK(testing::wait_until([&] { return broker.count(MqttType::Disconnect) == 1; }));
	CHECK_EQ(broker.count(MqttType::Puback), 1);	//end した 2 通目には PUBACK を返さない

	client.reset();
	CHECK(testing::wait_until([&] { return weak.expired(); }));

	//on_message の中で最後の参照を手放す: その場で壊れても落ちず、接続は閉じられる
	FakeBroker broker2;
	Recorder rec2;
	std::shared_ptr<MqttClient> holder = make_client(lt.loop(), options_for(broker2), rec2);
	std::weak_ptr<MqttClient> weak2 = holder;
	holder->on_message = [&holder, &rec2](std::string_view, std::string_view) {
		rec2.add("drop");
		holder.reset();
		rec2.add("released");	//holder を書き換えたことをテストのスレッドへ伝える (Recorder の mutex 経由)
	};
	holder->start();
	CHECK(rec2.wait("suback:ok"));
	broker2.send(publish_packet("owntracks/a/b", "x", 1, 5) + publish_packet("owntracks/a/b", "y", 1, 6));
	CHECK(rec2.wait("released"));
	CHECK(testing::wait_until([&] { return weak2.expired(); }));
	CHECK(testing::wait_until([&] { return broker2.closed.load() == 1; }));
	CHECK_EQ(rec2.count("drop"), 1);	//手放した後の 2 通目は届けない
	CHECK_EQ(broker2.connections.load(), 1);
}
