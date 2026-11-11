#include "portus/redis.hpp"

#include <chrono>
#include <mutex>

#include "portus/socket.hpp"

namespace portus {

struct RedisClient::Impl {
	std::string host;
	int port = 6379;
	int timeout_ms = 5000;

	std::mutex mu;
	socket_t fd = kInvalidSocket;
	RespParser parser;
	std::vector<RespReply> ready;

	void disconnect() {
		close_socket(fd);
		fd = kInvalidSocket;
		parser = RespParser();
		ready.clear();
	}

	void send_all(const std::string& data, std::chrono::steady_clock::time_point deadline) {
		std::size_t sent = 0;
		while (sent < data.size()) {
			const long n = socket_send(fd, data.data() + sent, data.size() - sent);
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
				continue;
			}
			if (!last_error_would_block()) throw std::runtime_error("redis connection closed");
			wait(kWrite, deadline);
		}
	}

	void wait(unsigned interest, std::chrono::steady_clock::time_point deadline) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0 || wait_socket(fd, interest, static_cast<int>(left)) == 0) throw std::runtime_error("redis timeout");
	}

	RespReply read_reply(std::chrono::steady_clock::time_point deadline) {
		char buf[16384];
		while (ready.empty()) {
			const long n = socket_recv(fd, buf, sizeof(buf));
			if (n > 0) {
				ready = parser.push(std::string_view(buf, static_cast<std::size_t>(n)));
				continue;
			}
			if (n == 0 || !last_error_would_block()) throw std::runtime_error("redis connection closed");
			wait(kRead, deadline);
		}
		RespReply reply = std::move(ready.front());
		ready.erase(ready.begin());
		return reply;
	}
};

RedisClient::RedisClient(std::string host, int port, int timeout_ms) : impl_(new Impl()) {
	impl_->host = std::move(host);
	impl_->port = port;
	impl_->timeout_ms = timeout_ms;
}

RedisClient::~RedisClient() {
	impl_->disconnect();
}

Json RedisClient::command(const std::vector<std::string>& args) {
	std::lock_guard<std::mutex> lock(impl_->mu);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(impl_->timeout_ms);
	if (impl_->fd == kInvalidSocket) impl_->fd = connect_tcp(impl_->host, impl_->port, impl_->timeout_ms);

	RespReply reply;
	try {
		impl_->send_all(encode_command(args), deadline);
		reply = impl_->read_reply(deadline);
	} catch (...) {
		//途中で失敗した接続は応答の対応が取れないので捨てる
		impl_->disconnect();
		throw;
	}
	if (reply.is_error) throw RedisReplyError(reply.value.as_string());
	return reply.value;
}

std::optional<std::string> RedisClient::get(const std::string& key) {
	const Json v = command({ "GET", key });
	if (v.is_null()) return std::nullopt;
	return v.as_string();
}

void RedisClient::set(const std::string& key, const std::string& value) {
	command({ "SET", key, value });
}

long long RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
	return static_cast<long long>(command({ "HSET", key, field, value }).as_number());
}

std::map<std::string, std::string> RedisClient::hgetall(const std::string& key) {
	const Json v = command({ "HGETALL", key });
	const Json::Array& list = v.items();
	std::map<std::string, std::string> out;
	for (std::size_t i = 0; i + 1 < list.size(); i += 2) out[list[i].as_string()] = list[i + 1].as_string();
	return out;
}

void RedisClient::quit() {
	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->disconnect();
}

}
