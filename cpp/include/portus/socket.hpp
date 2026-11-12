#pragma once
//OS のソケット API の薄い包み (POSIX / Winsock)。ここより上の層は OS のヘッダを見ない
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace portus {

#ifdef _WIN32
using socket_t = std::uintptr_t;
constexpr socket_t kInvalidSocket = ~static_cast<socket_t>(0);
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

//待ち受ける / 起きた事象
enum : unsigned { kRead = 1, kWrite = 2, kError = 4 };

//解決済みの接続先 1 件
struct SockAddr {
	unsigned char data[128];
	int len = 0;
	int family = 0;
};

struct Listener {
	socket_t fd = kInvalidSocket;
	int port = 0;
};

void net_init();	//Winsock の初期化 (他 OS では何もしない)。何度呼んでもよい
void close_socket(socket_t fd);
void set_nonblocking(socket_t fd);
void set_nodelay(socket_t fd);
void shutdown_write(socket_t fd);
int socket_error(socket_t fd);	//SO_ERROR
bool last_error_would_block();

//戻り値: 転送バイト数 / 0 = 相手が閉じた (recv) / -1 = エラー (last_error_would_block で判別)
//  send は len より少なく送ることがある (Windows では 1 回 64 KB まで)。呼び出し側は残りを送り直す
long socket_recv(socket_t fd, char* buf, std::size_t len);
long socket_send(socket_t fd, const char* buf, std::size_t len);

//失敗時は std::runtime_error
Listener listen_tcp(const std::string& host, int port, int backlog = 128);
socket_t accept_tcp(socket_t listener, std::string* peer);	//無ければ kInvalidSocket (待ちか失敗かは直後の last_error_would_block で判別)
std::vector<SockAddr> resolve_tcp(const std::string& host, int port);
socket_t start_connect(const SockAddr& addr, bool* in_progress);	//非ブロッキング接続。失敗は kInvalidSocket
socket_t connect_tcp(const std::string& host, int port, int timeout_ms);	//接続完了まで待つ。失敗は例外

//1 本のソケットを待つ。戻り値は事象のビット、0 はタイムアウト
unsigned wait_socket(socket_t fd, unsigned interest, int timeout_ms);

//イベントループを起こすための接続済みソケット対
void make_wake_pair(socket_t out[2]);

}
