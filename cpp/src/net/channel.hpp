#pragma once
//1 本の接続 (平文または TLS) を、全体の期限付きで読み書きする (src 内部専用)
#include <chrono>
#include <cstddef>
#include <string>

#include "portus/socket.hpp"

namespace portus {

class Channel {
public:
	using Deadline = std::chrono::steady_clock::time_point;

	//接続する。tls なら証明書とホスト名を検証してハンドシェイクまで終える。失敗は FetchError
	Channel(const std::string& host, int port, bool tls, const std::string& ca_file, Deadline deadline);
	~Channel();
	Channel(const Channel&) = delete;
	Channel& operator=(const Channel&) = delete;

	void write_all(const std::string& data);
	std::size_t read(char* buf, std::size_t len);	//0 = 相手が正常に閉じた

private:
	void wait(unsigned interest);

	socket_t fd_ = kInvalidSocket;
	void* ssl_ = nullptr;
	void* owned_ctx_ = nullptr;
	Deadline deadline_;
};

}
