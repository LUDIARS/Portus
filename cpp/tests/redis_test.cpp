#include "portus/redis.hpp"
#include "portus/socket.hpp"
#include "test.hpp"

using namespace portus;

TEST(resp_codec, "Redis: RESP のエンコードと分割受信の解析") {
	CHECK_EQ(encode_command({ "SET", "k", "あ" }), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nあ\r\n");

	RespParser p;
	const std::string bytes = "+OK\r\n$3\r\nあ\r\n*2\r\n$1\r\na\r\n$-1\r\n:5\r\n-ERR bad\r\n";
	std::vector<RespReply> replies = p.push(bytes.substr(0, 9));
	for (RespReply& r : p.push(bytes.substr(9))) replies.push_back(std::move(r));
	CHECK_EQ(replies.size(), 5u);
	CHECK_EQ(replies[0].value.as_string(), "OK");
	CHECK_EQ(replies[1].value.as_string(), "あ");
	CHECK(replies[2].value == Json::array({ "a", nullptr }));
	CHECK_EQ(replies[3].value.as_number(), 5);
	CHECK(replies[4].is_error);
	CHECK_EQ(replies[4].value.as_string(), "ERR bad");
}

TEST(resp_reject, "Redis: 異常な長さ・不明な型・深すぎる入れ子は拒否する") {
	CHECK_THROWS(RespParser().push("$999999999999\r\n"));
	CHECK_THROWS(RespParser().push("*99999999\r\n"));
	CHECK_THROWS(RespParser().push("?x\r\n"));
	CHECK_THROWS(RespParser().push(":12x\r\n"));
	std::string nested;
	for (int i = 0; i < 100; ++i) nested += "*1\r\n";
	CHECK_THROWS(RespParser().push(nested));
}

namespace {

//最小の Redis もどき。1 接続だけ受け、コマンドに決まった応答を返す
struct FakeRedis {
	Listener listener;
	std::thread thread;
	std::atomic<bool> stop{false};

	FakeRedis() : listener(listen_tcp("127.0.0.1", 0)) {
		thread = std::thread([this] { serve(); });
	}
	~FakeRedis() {
		stop = true;
		thread.join();
		close_socket(listener.fd);
	}

	static std::string reply_for(const Json::Array& cmd) {
		const std::string& name = cmd[0].as_string();
		if (name == "SET") return "+OK\r\n";
		if (name == "GET") return cmd[1].as_string() == "k" ? "$1\r\nv\r\n" : "$-1\r\n";
		if (name == "HSET") return ":1\r\n";
		if (name == "HGETALL") return "*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n";
		return "-ERR unknown command\r\n";
	}

	void serve() {
		socket_t fd = kInvalidSocket;
		RespParser parser;
		char buf[4096];
		while (!stop) {
			if (fd == kInvalidSocket) {
				if (wait_socket(listener.fd, kRead, 20)) fd = accept_tcp(listener.fd, nullptr);
				continue;
			}
			if (!wait_socket(fd, kRead, 20)) continue;
			const long n = socket_recv(fd, buf, sizeof(buf));
			if (n <= 0) {
				close_socket(fd);
				fd = kInvalidSocket;
				parser = RespParser();
				continue;
			}
			for (const RespReply& cmd : parser.push(std::string_view(buf, static_cast<std::size_t>(n)))) {
				const std::string out = reply_for(cmd.value.items());
				socket_send(fd, out.data(), out.size());
			}
		}
		close_socket(fd);
	}
};

}

TEST(redis_client, "Redis: クライアントの往復、エラー応答、切断後の再接続") {
	FakeRedis server;
	RedisClient redis("127.0.0.1", server.listener.port, 2000);

	redis.set("k", "v");
	CHECK_EQ(redis.get("k").value_or("<none>"), "v");
	CHECK(!redis.get("missing").has_value());
	CHECK_EQ(redis.hset("h", "a", "1"), 1);
	const auto all = redis.hgetall("h");
	CHECK_EQ(all.size(), 2u);
	CHECK_EQ(all.at("b"), "2");

	bool reply_error = false;
	try {
		redis.command({ "NOPE" });
	} catch (const RedisReplyError& ex) {
		reply_error = std::string(ex.what()) == "ERR unknown command";
	}
	CHECK(reply_error);
	CHECK_EQ(redis.get("k").value_or("<none>"), "v");	//エラー応答の後も同じ接続を使える

	redis.quit();
	CHECK_EQ(redis.get("k").value_or("<none>"), "v");	//次のコマンドで張り直す
}

TEST(redis_unreachable, "Redis: 繋がらないときは例外で返る") {
	const Listener l = listen_tcp("127.0.0.1", 0);
	close_socket(l.fd);
	RedisClient redis("127.0.0.1", l.port, 500);
	CHECK_THROWS(redis.get("k"));
}

TEST(resp_large_array_chunks, "Redis: 大きな配列が少しずつ届いても、届くたびに先頭から読み直さない") {
	const int n = 200000;
	std::string raw = "*" + std::to_string(n) + "\r\n";
	for (int i = 0; i < n; ++i) raw += "$1\r\nx\r\n";
	raw += ":5\r\n";

	RespParser parser;
	std::vector<RespReply> replies;
	const auto started = std::chrono::steady_clock::now();
	for (std::size_t pos = 0; pos < raw.size(); pos += 16384) {
		for (RespReply& r : parser.push(std::string_view(raw).substr(pos, 16384))) replies.push_back(std::move(r));
	}
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(ms < 1000 * testing::kTimeScale);	//読み直すと 2 乗時間で十数秒かかる
	CHECK_EQ(replies.size(), 2u);
	CHECK_EQ(replies[0].value.items().size(), static_cast<std::size_t>(n));
	CHECK_EQ(replies[1].value.as_number(), 5);

	//入れ子の配列も分割の位置に関係なく組み立てられる
	const std::string nested = "*2\r\n*2\r\n:1\r\n$2\r\nab\r\n*0\r\n-ERR x\r\n";
	RespParser byte_wise;
	std::vector<RespReply> got;
	for (char c : nested) {
		for (RespReply& r : byte_wise.push(std::string_view(&c, 1))) got.push_back(std::move(r));
	}
	CHECK_EQ(got.size(), 2u);
	CHECK_EQ(got[0].value.dump(), "[[1,\"ab\"],[]]");
	CHECK(got[1].is_error);
}

TEST(resp_integer_overflow, "Redis: 桁あふれする整数は拒否する") {
	CHECK_THROWS(RespParser().push(":9999999999999999999\r\n"));
	CHECK_THROWS(RespParser().push(":-9999999999999999999\r\n"));
	CHECK_EQ(RespParser().push(":-999999999999999999\r\n")[0].value.as_number(), -999999999999999999.0);
}
