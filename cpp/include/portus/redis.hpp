#pragma once
//Redis クライアント (RESP2)。ハンドラ (ワーカースレッド) から同期的に呼ぶ
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "portus/json.hpp"

namespace portus {

//Redis がエラー応答 (-ERR ...) を返した
class RedisReplyError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

//応答 1 つ。値は Json で持つ (文字列 / 数値 / null / 配列)
struct RespReply {
	bool is_error = false;
	Json value;
};

//コマンドを RESP 配列にする
std::string encode_command(const std::vector<std::string>& args);

//受信バイト列から応答を順に取り出す。壊れた応答・異常な長さ・深すぎる入れ子は std::runtime_error
class RespParser {
public:
	std::vector<RespReply> push(std::string_view chunk);

private:
	struct Pending {	//要素を待っている配列
		Json items;
		long long remaining;
	};
	bool parse_one(std::size_t offset, RespReply& out, long long& count, std::size_t& next) const;
	std::string buffer_;
	std::vector<Pending> stack_;
};

//接続は最初のコマンドで張る。切れたら次のコマンドで張り直す。複数スレッドから呼んでよい (1 本を順に使う)
class RedisClient {
public:
	explicit RedisClient(std::string host = "127.0.0.1", int port = 6379, int timeout_ms = 5000);
	~RedisClient();
	RedisClient(const RedisClient&) = delete;
	RedisClient& operator=(const RedisClient&) = delete;

	//任意のコマンド。エラー応答は RedisReplyError、通信の失敗・時間切れは std::runtime_error
	Json command(const std::vector<std::string>& args);

	std::optional<std::string> get(const std::string& key);
	void set(const std::string& key, const std::string& value);
	long long hset(const std::string& key, const std::string& field, const std::string& value);
	std::map<std::string, std::string> hgetall(const std::string& key);
	void quit();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
