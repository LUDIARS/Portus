#include "portus/redis.hpp"

#include <cstdlib>

namespace portus {

namespace {

const long long kMaxBulk = 64LL * 1024 * 1024;
const long long kMaxArray = 1024 * 1024;
const int kMaxDepth = 32;

//10 進整数として読めたか
bool parse_int(std::string_view text, long long& out) {
	if (text.empty() || text.size() > 19) return false;
	std::size_t i = text[0] == '-' ? 1 : 0;
	if (i == text.size()) return false;
	long long n = 0;
	for (; i < text.size(); ++i) {
		if (text[i] < '0' || text[i] > '9') return false;
		n = n * 10 + (text[i] - '0');
	}
	out = text[0] == '-' ? -n : n;
	return true;
}

}

std::string encode_command(const std::vector<std::string>& args) {
	std::string out = "*" + std::to_string(args.size()) + "\r\n";
	for (const std::string& arg : args) {
		out += "$" + std::to_string(arg.size()) + "\r\n";
		out += arg;
		out += "\r\n";
	}
	return out;
}

std::vector<RespReply> RespParser::push(std::string_view chunk) {
	buffer_.append(chunk);
	std::vector<RespReply> replies;
	std::size_t consumed = 0;
	for (;;) {
		RespReply reply;
		std::size_t next = 0;
		if (!parse(consumed, 0, reply, next)) break;
		replies.push_back(std::move(reply));
		consumed = next;
	}
	buffer_.erase(0, consumed);
	return replies;
}

//offset から 1 つ読む。足りなければ false
bool RespParser::parse(std::size_t offset, int depth, RespReply& out, std::size_t& next) const {
	if (depth > kMaxDepth) throw std::runtime_error("RESP nesting too deep");
	if (buffer_.size() <= offset) return false;

	const char type = buffer_[offset];
	const std::size_t end = buffer_.find("\r\n", offset + 1);
	if (end == std::string::npos) {
		if (buffer_.size() - offset > 64 * 1024) throw std::runtime_error("RESP line too long");
		return false;
	}
	const std::string_view line = std::string_view(buffer_).substr(offset + 1, end - offset - 1);
	next = end + 2;

	switch (type) {
	case '+':
		out.value = Json(line);
		return true;
	case '-':
		out.is_error = true;
		out.value = Json(line);
		return true;
	case ':': {
		long long n = 0;
		if (!parse_int(line, n)) throw std::runtime_error("invalid RESP integer");
		out.value = Json(n);
		return true;
	}
	case '$': {
		long long len = 0;
		if (!parse_int(line, len) || len > kMaxBulk) throw std::runtime_error("invalid bulk length");
		if (len < 0) {
			out.value = Json(nullptr);
			return true;
		}
		const std::size_t size = static_cast<std::size_t>(len);
		if (buffer_.size() < next + size + 2) return false;
		out.value = Json(buffer_.substr(next, size));
		next += size + 2;
		return true;
	}
	case '*': {
		long long count = 0;
		if (!parse_int(line, count) || count > kMaxArray) throw std::runtime_error("invalid array length");
		if (count < 0) {
			out.value = Json(nullptr);
			return true;
		}
		Json items = Json::array();
		for (long long i = 0; i < count; ++i) {
			RespReply item;
			std::size_t item_next = 0;
			if (!parse(next, depth + 1, item, item_next)) return false;
			//入れ子のエラーは文字列として持つ
			items.push_back(std::move(item.value));
			next = item_next;
		}
		out.value = std::move(items);
		return true;
	}
	default:
		throw std::runtime_error(std::string("unknown RESP type ") + type);
	}
}

}
