#pragma once
//WebSocket (RFC 6455) のサーバ / クライアント。拡張 (permessage-deflate など) は持たない
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "portus/event_loop.hpp"
#include "portus/http_message.hpp"

namespace portus {

class TcpConn;

enum class Opcode { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

struct Frame {
	bool fin = true;
	Opcode opcode = Opcode::Text;
	std::string payload;
};

class ProtocolError : public std::runtime_error {
public:
	ProtocolError(const std::string& message, int close_code_) : std::runtime_error(message), close_code(close_code_) {}
	int close_code;
};

//フレームを組み立てる。クライアント送信時は mask = true
std::string encode_frame(Opcode opcode, std::string_view payload, bool mask, bool fin = true);

//受信バイト列からフレームを取り出す。不正なフレームは ProtocolError
class FrameParser {
public:
	//expect_masked: サーバ側は true (クライアントは必ずマスクする)、クライアント側は false
	FrameParser(bool expect_masked, std::size_t max_payload) : expect_masked_(expect_masked), max_payload_(max_payload) {}
	std::vector<Frame> push(std::string_view chunk);

private:
	bool next(Frame& out);

	bool expect_masked_;
	std::size_t max_payload_;
	std::string buffer_;
	std::size_t pos_ = 0;
};

std::string accept_key(std::string_view key);	//Sec-WebSocket-Accept の値
std::string generate_key();						//クライアント用の Sec-WebSocket-Key
//サーバ側: アップグレード要求を検証し、問題があれば理由を返す
std::optional<std::string> validate_upgrade(const std::string& method, const Headers& headers);
std::string build_accept_response(std::string_view key);

constexpr std::size_t kDefaultMaxPayload = 1024 * 1024;
constexpr std::size_t kDefaultMaxBuffered = 16 * 1024 * 1024;

//WebSocket 接続 1 本
//  コールバックはイベントループのスレッドで呼ばれる (中でブロックしない。重い処理は ThreadPool へ)
//  send / ping / close / terminate はどのスレッドからでも呼べる
class WebSocket : public std::enable_shared_from_this<WebSocket> {
public:
	enum State { CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3 };

	std::function<void()> on_open;	//クライアント接続のみ
	std::function<void(std::string_view data, bool is_binary)> on_message;
	std::function<void(std::string_view data)> on_ping;
	std::function<void(std::string_view data)> on_pong;
	std::function<void(int code, std::string_view reason)> on_close;
	std::function<void(const std::string& message)> on_error;

	//is_client: 自分がクライアントか (送信フレームをマスクする側か)
	WebSocket(EventLoop& loop, bool is_client, std::size_t max_payload);
	~WebSocket();

	void send(std::string_view text);
	void send_binary(std::string_view data);
	void ping(std::string_view data = "");
	void close(int code = 1000, std::string_view reason = "");
	void terminate();
	State ready_state() const;

	//以下はサーバ / クライアントの接続処理が使う (ループスレッド)
	void attach(std::shared_ptr<TcpConn> conn);	//ハンドシェイク済みの接続を結び付けて OPEN にする
	void receive(std::string_view data);		//ハンドシェイクと一緒に届いていた先読み分を渡す
	void fail_connect(const std::string& message);	//接続できなかったことを伝える

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct WebSocketServerOptions {
	std::size_t max_payload = kDefaultMaxPayload;	//超過は 1009 で切断
	std::size_t max_buffered = kDefaultMaxBuffered;	//送信待ちがこれを超えた接続は切る (遅い相手にメモリを取られない)
	std::size_t max_connections = 1024;
	int handshake_timeout_ms = 10000;

	//空でなければ、Origin ヘッダ付きの要求はこの中の Origin だけ受ける (それ以外は 403)
	//ブラウザは WebSocket に同一オリジン制限を掛けないので、ローカルサービスでは設定すること
	std::vector<std::string> allowed_origins;
};

//WebSocket サーバ。WebSocket 以外の HTTP 要求には 426 を返す
class WebSocketServer {
public:
	std::function<void(const std::shared_ptr<WebSocket>& ws, const RequestHead& request)> on_connection;

	WebSocketServer(EventLoop& loop, WebSocketServerOptions options = {});
	~WebSocketServer();
	WebSocketServer(const WebSocketServer&) = delete;
	WebSocketServer& operator=(const WebSocketServer&) = delete;

	int listen(int port, const std::string& host = "127.0.0.1");	//待ち受けたポートを返す
	void close();	//待ち受けを止め、全接続を切る

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

//ws:// へ接続する。戻り値はすぐ返り、接続できたら on_open、できなければ on_error と on_close(1006) が呼ばれる
//コールバックは戻り値に対して (ループスレッドで、または run 開始前に) 設定する
std::shared_ptr<WebSocket> connect_websocket(EventLoop& loop, const std::string& url, std::size_t max_payload = kDefaultMaxPayload);

}
