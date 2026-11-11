#include "portus/redis.hpp"

#include <cstdlib>

namespace portus {

namespace {

const long long kMaxBulk = 64LL * 1024 * 1024;
const long long kMaxArray = 1024 * 1024;
const int kMaxDepth = 32;

//10 進整数として読めたか。18 桁までにして long long の桁あふれ (未定義動作) を起こさない
bool parse_int(std::string_view text, long long& out) {
	if (text.empty()) return false;
	std::size_t i = text[0] == '-' ? 1 : 0;
	if (i == text.size() || text.size() - i > 18) return false;
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
	std::size_t pos = 0;
	for (;;) {
		//組み立て途中の配列は stack_ に残して続きから読む。届くたびに先頭から読み直すと大きな配列で 2 乗時間になる
		if (stack_.size() > static_cast<std::size_t>(kMaxDepth)) throw std::runtime_error("RESP nesting too deep");
		RespReply reply;
		long long count = -1;
		std::size_t next = 0;
		if (!parse_one(pos, reply, count, next)) break;
		pos = next;
		if (count > 0) {
			stack_.push_back(Pending{ Json::array(), count });
			continue;
		}
		//値が 1 つ揃った。埋まった配列を順に親へ畳む
		bool complete = true;
		while (!stack_.empty()) {
			Pending& top = stack_.back();
			top.items.push_back(std::move(reply.value));	//入れ子のエラーは文字列として持つ
			if (--top.remaining > 0) {
				complete = false;
				break;
			}
			reply = RespReply();
			reply.value = std::move(top.items);
			stack_.pop_back();
		}
		if (complete) replies.push_back(std::move(reply));
	}
	buffer_.erase(0, pos);
	return replies;
}

//offset から値 1 つ (または要素数 count > 0 の配列の頭) を読む。足りなければ false
bool RespParser::parse_one(std::size_t offset, RespReply& out, long long& count, std::size_t& next) const {
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
		if (!parse_int(line, count) || count > kMaxArray) throw std::runtime_error("invalid array length");
		if (count < 0) out.value = Json(nullptr);
		else if (count == 0) out.value = Json::array();
		return true;
	}
	default:
		throw std::runtime_error(std::string("unknown RESP type ") + type);
	}
}

}
