#include "portus/id.hpp"

#include "portus/crypto.hpp"

namespace portus {

std::string uuidv4() {
	//乱数はスレッドごとにまとめて取り、16 バイトずつ使う (1 件ごとに CSPRNG を呼ぶと要求あたりのコストになる)
	thread_local std::string pool;
	thread_local std::size_t used = 0;
	if (used + 16 > pool.size()) {
		pool = random_bytes(1024);
		used = 0;
	}
	std::string b = pool.substr(used, 16);
	used += 16;
	b[6] = static_cast<char>((b[6] & 0x0F) | 0x40);
	b[8] = static_cast<char>((b[8] & 0x3F) | 0x80);
	const std::string h = hex(b);
	return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

}
