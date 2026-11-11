#include "portus/ws.hpp"

#include "portus/crypto.hpp"

namespace portus {

static bool is_control(int opcode) {
	return (opcode & 0x8) != 0;
}

static bool known_opcode(int opcode) {
	return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 || opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
}

std::string encode_frame(Opcode opcode, std::string_view payload, bool mask, bool fin) {
	const std::size_t len = payload.size();
	std::string out;
	out.reserve(len + 14);
	out.push_back(static_cast<char>((fin ? 0x80 : 0) | static_cast<int>(opcode)));

	const int mask_bit = mask ? 0x80 : 0;
	if (len >= 65536) {
		out.push_back(static_cast<char>(mask_bit | 127));
		for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> shift) & 0xFF));
	} else if (len >= 126) {
		out.push_back(static_cast<char>(mask_bit | 126));
		out.push_back(static_cast<char>((len >> 8) & 0xFF));
		out.push_back(static_cast<char>(len & 0xFF));
	} else {
		out.push_back(static_cast<char>(mask_bit | static_cast<int>(len)));
	}

	if (!mask) {
		out.append(payload);
		return out;
	}
	const std::string key = random_bytes(4);
	out.append(key);
	const std::size_t start = out.size();
	out.append(payload);
	for (std::size_t i = 0; i < len; ++i) out[start + i] = static_cast<char>(out[start + i] ^ key[i & 3]);
	return out;
}

std::vector<Frame> FrameParser::push(std::string_view chunk) {
	if (pos_ > 0) {
		buffer_.erase(0, pos_);
		pos_ = 0;
	}
	buffer_.append(chunk);

	std::vector<Frame> frames;
	Frame frame;
	while (next(frame)) frames.push_back(std::move(frame));
	return frames;
}

bool FrameParser::next(Frame& out) {
	const std::size_t avail = buffer_.size() - pos_;
	if (avail < 2) return false;
	const unsigned char* buf = reinterpret_cast<const unsigned char*>(buffer_.data()) + pos_;

	const unsigned char b0 = buf[0];
	const unsigned char b1 = buf[1];
	const bool fin = (b0 & 0x80) != 0;
	if ((b0 & 0x70) != 0) throw ProtocolError("RSV bits must be 0", 1002);
	const int opcode = b0 & 0x0F;
	if (!known_opcode(opcode)) throw ProtocolError("unknown opcode " + std::to_string(opcode), 1002);

	const bool masked = (b1 & 0x80) != 0;
	if (masked != expect_masked_) throw ProtocolError(masked ? "unexpected mask" : "frame must be masked", 1002);

	std::uint64_t len = b1 & 0x7F;
	std::size_t offset = 2;
	if (len == 126) {
		if (avail < 4) return false;
		len = (static_cast<std::uint64_t>(buf[2]) << 8) | buf[3];
		offset = 4;
	} else if (len == 127) {
		if (avail < 10) return false;
		len = 0;
		for (int i = 0; i < 8; ++i) len = (len << 8) | buf[2 + i];
		offset = 10;
	}

	//本体を待たずに、長さだけで上限超過を判断する
	if (is_control(opcode) && (len > 125 || !fin)) throw ProtocolError("invalid control frame", 1002);
	if (len > max_payload_) throw ProtocolError("payload too large", 1009);

	const std::size_t mask_len = masked ? 4 : 0;
	const std::size_t body = static_cast<std::size_t>(len);
	if (avail < offset + mask_len + body) return false;

	out.fin = fin;
	out.opcode = static_cast<Opcode>(opcode);
	out.payload.assign(reinterpret_cast<const char*>(buf) + offset + mask_len, body);
	if (masked) {
		const unsigned char* key = buf + offset;
		for (std::size_t i = 0; i < body; ++i) out.payload[i] = static_cast<char>(out.payload[i] ^ key[i & 3]);
	}
	pos_ += offset + mask_len + body;
	return true;
}

}
