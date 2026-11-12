#include "portus/mqtt.hpp"

#include "portus/util.hpp"

namespace portus {

namespace {

const std::size_t kMaxString = 65535;	//UTF-8 文字列・バイナリは 2 バイトの長さで表す

void put_u16(std::string& out, std::size_t v) {
	out.push_back(static_cast<char>((v >> 8) & 0xFF));
	out.push_back(static_cast<char>(v & 0xFF));
}

void put_string(std::string& out, std::string_view s) {
	if (s.size() > kMaxString) throw std::invalid_argument("mqtt string too long");
	put_u16(out, s.size());
	out.append(s);
}

std::string with_header(unsigned first_byte, const std::string& body) {
	std::string out;
	out.push_back(static_cast<char>(first_byte));
	out.append(encode_mqtt_remaining_length(body.size()));
	out.append(body);
	return out;
}

std::uint16_t get_u16(const std::string& s, std::size_t at) {
	return static_cast<std::uint16_t>((static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]));
}

//固定ヘッダのフラグは種別ごとに決まっている (PUBLISH 以外)。違えば受け取らない (3.1.1 §2.2.2)
bool valid_flags(int type, unsigned flags) {
	if (type == static_cast<int>(MqttType::Publish)) return ((flags >> 1) & 0x3) != 3;
	const bool fixed_two = type == static_cast<int>(MqttType::Pubrel) || type == static_cast<int>(MqttType::Subscribe) || type == static_cast<int>(MqttType::Unsubscribe);
	return flags == (fixed_two ? 0x2u : 0x0u);
}

}

std::string encode_mqtt_remaining_length(std::size_t n) {
	if (n > kMqttMaxRemainingLength) throw std::invalid_argument("mqtt packet too large");
	std::string out;
	do {
		unsigned char b = static_cast<unsigned char>(n % 128);
		n /= 128;
		if (n > 0) b |= 0x80;
		out.push_back(static_cast<char>(b));
	} while (n > 0);
	return out;
}

std::string encode_mqtt_connect(std::string_view client_id, std::string_view username, std::string_view password, int keep_alive_sec) {
	if (keep_alive_sec < 0 || keep_alive_sec > 65535) throw std::invalid_argument("keep_alive_sec out of range");
	//3.1.1 は username 無しの password を許さない (§3.1.2.9)
	if (username.empty() && !password.empty()) throw std::invalid_argument("password requires username");

	unsigned flags = 0x02;	//clean session。購読は接続ごとに張り直すのでセッションを残さない
	if (!username.empty()) flags |= 0x80;
	if (!password.empty()) flags |= 0x40;

	std::string body;
	put_string(body, "MQTT");
	body.push_back(0x04);	//プロトコルレベル 4 = 3.1.1
	body.push_back(static_cast<char>(flags));
	put_u16(body, static_cast<std::size_t>(keep_alive_sec));
	put_string(body, client_id);
	if (!username.empty()) put_string(body, username);
	if (!password.empty()) put_string(body, password);
	return with_header(0x10, body);
}

std::string encode_mqtt_subscribe(std::uint16_t packet_id, std::string_view topic_filter, int qos) {
	if (packet_id == 0) throw std::invalid_argument("packet id must not be 0");
	if (qos < 0 || qos > 1) throw std::invalid_argument("qos must be 0 or 1");
	std::string body;
	put_u16(body, packet_id);
	put_string(body, topic_filter);
	body.push_back(static_cast<char>(qos));
	return with_header(0x82, body);
}

std::string encode_mqtt_puback(std::uint16_t packet_id) {
	std::string body;
	put_u16(body, packet_id);
	return with_header(0x40, body);
}

std::string encode_mqtt_pingreq() {
	return std::string("\xC0\x00", 2);
}

std::string encode_mqtt_disconnect() {
	return std::string("\xE0\x00", 2);
}

MqttConnack decode_mqtt_connack(const MqttPacket& packet) {
	if (packet.type != MqttType::Connack || packet.body.size() != 2) throw MqttProtocolError("invalid CONNACK");
	const unsigned char ack_flags = static_cast<unsigned char>(packet.body[0]);
	if (ack_flags & 0xFE) throw MqttProtocolError("invalid CONNACK flags");
	MqttConnack out;
	out.session_present = (ack_flags & 0x01) != 0;
	out.return_code = static_cast<unsigned char>(packet.body[1]);
	return out;
}

MqttSuback decode_mqtt_suback(const MqttPacket& packet) {
	if (packet.type != MqttType::Suback || packet.body.size() < 3) throw MqttProtocolError("invalid SUBACK");
	MqttSuback out;
	out.packet_id = get_u16(packet.body, 0);
	for (std::size_t i = 2; i < packet.body.size(); ++i) {
		const int code = static_cast<unsigned char>(packet.body[i]);
		if (code != 0x00 && code != 0x01 && code != 0x02 && code != 0x80) throw MqttProtocolError("invalid SUBACK return code");
		out.return_codes.push_back(code);
	}
	return out;
}

MqttPublish decode_mqtt_publish(const MqttPacket& packet) {
	if (packet.type != MqttType::Publish) throw MqttProtocolError("not a PUBLISH");
	MqttPublish out;
	out.dup = (packet.flags & 0x8) != 0;
	out.qos = static_cast<int>((packet.flags >> 1) & 0x3);
	out.retain = (packet.flags & 0x1) != 0;
	if (out.qos == 3) throw MqttProtocolError("invalid PUBLISH qos");
	if (out.qos == 0 && out.dup) throw MqttProtocolError("DUP set on QoS 0 PUBLISH");

	const std::string& b = packet.body;
	if (b.size() < 2) throw MqttProtocolError("truncated PUBLISH");
	const std::size_t topic_len = get_u16(b, 0);
	std::size_t pos = 2 + topic_len;
	if (b.size() < pos) throw MqttProtocolError("truncated PUBLISH topic");
	out.topic = std::string_view(b).substr(2, topic_len);
	//トピック名は空でない UTF-8 で、ワイルドカードと U+0000 を含まない (§4.7)
	if (out.topic.empty() || !utf8_valid(out.topic) || out.topic.find_first_of(std::string_view("+#\0", 3)) != std::string_view::npos) {
		throw MqttProtocolError("invalid PUBLISH topic");
	}
	if (out.qos > 0) {
		if (b.size() < pos + 2) throw MqttProtocolError("truncated PUBLISH packet id");
		out.packet_id = get_u16(b, pos);
		if (out.packet_id == 0) throw MqttProtocolError("PUBLISH packet id must not be 0");
		pos += 2;
	}
	out.payload = std::string_view(b).substr(pos);
	return out;
}

std::vector<MqttPacket> MqttPacketParser::push(std::string_view chunk) {
	if (pos_ > 0) {
		buffer_.erase(0, pos_);
		pos_ = 0;
	}
	buffer_.append(chunk);

	std::vector<MqttPacket> packets;
	MqttPacket packet;
	while (next(packet)) packets.push_back(std::move(packet));
	return packets;
}

bool MqttPacketParser::next(MqttPacket& out) {
	const std::size_t avail = buffer_.size() - pos_;
	if (avail < 2) return false;
	const unsigned char* buf = reinterpret_cast<const unsigned char*>(buffer_.data()) + pos_;

	const int type = buf[0] >> 4;
	const unsigned flags = buf[0] & 0x0F;
	if (type < 1 || type > 14) throw MqttProtocolError("invalid packet type " + std::to_string(type));
	if (!valid_flags(type, flags)) throw MqttProtocolError("invalid fixed header flags");

	//残り長さは最大 4 バイト。5 バイト目を待たずに、4 バイト目の継続ビットで拒否する
	std::size_t remaining = 0;
	std::size_t used = 0;
	for (;;) {
		if (used == 4) throw MqttProtocolError("remaining length too long");
		if (avail < 2 + used) return false;
		const unsigned char b = buf[1 + used];
		remaining |= static_cast<std::size_t>(b & 0x7F) << (7 * used);
		++used;
		if (!(b & 0x80)) break;
	}

	//本体を待たずに、長さだけで上限超過を判断する
	const std::size_t header = 1 + used;
	if (remaining > max_packet_ || header + remaining > max_packet_) throw MqttProtocolError("packet too large");
	if (avail < header + remaining) return false;

	out.type = static_cast<MqttType>(type);
	out.flags = flags;
	out.body.assign(reinterpret_cast<const char*>(buf) + header, remaining);
	pos_ += header + remaining;
	return true;
}

}
