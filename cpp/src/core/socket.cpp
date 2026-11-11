#include "portus/socket.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "platform.hpp"

namespace portus {

#ifdef _WIN32
using native_t = SOCKET;
static int last_error() { return WSAGetLastError(); }
#else
using native_t = int;
static int last_error() { return errno; }
#endif

void net_init() {
	static std::once_flag once;
	std::call_once(once, [] {
#ifdef _WIN32
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#else
		//切れた相手への書き込みはエラーで受け取る (OpenSSL 経由の書き込みは MSG_NOSIGNAL を付けられない)
		std::signal(SIGPIPE, SIG_IGN);
#endif
	});
}

void close_socket(socket_t fd) {
	if (fd == kInvalidSocket) return;
#ifdef _WIN32
	closesocket(static_cast<native_t>(fd));
#else
	::close(fd);
#endif
}

void set_nonblocking(socket_t fd) {
#ifdef _WIN32
	u_long on = 1;
	ioctlsocket(static_cast<native_t>(fd), FIONBIO, &on);
#else
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
}

void set_nodelay(socket_t fd) {
	int on = 1;
	setsockopt(static_cast<native_t>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

void shutdown_write(socket_t fd) {
#ifdef _WIN32
	shutdown(static_cast<native_t>(fd), SD_SEND);
#else
	shutdown(fd, SHUT_WR);
#endif
}

int socket_error(socket_t fd) {
	int err = 0;
	socklen_t len = sizeof(err);
	if (getsockopt(static_cast<native_t>(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return last_error();
	return err;
}

bool last_error_would_block() {
#ifdef _WIN32
	return last_error() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static bool last_error_in_progress() {
#ifdef _WIN32
	return last_error() == WSAEWOULDBLOCK || last_error() == WSAEINPROGRESS;
#else
	return errno == EINPROGRESS || errno == EINTR;
#endif
}

long socket_recv(socket_t fd, char* buf, std::size_t len) {
	return static_cast<long>(::recv(static_cast<native_t>(fd), buf, static_cast<int>(len), 0));
}

long socket_send(socket_t fd, const char* buf, std::size_t len) {
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL;	//相手が切れていても SIGPIPE で落ちない
#else
	const int flags = 0;
#endif
	return static_cast<long>(::send(static_cast<native_t>(fd), buf, static_cast<int>(len), flags));
}

std::vector<SockAddr> resolve_tcp(const std::string& host, int port) {
	net_init();
	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* list = nullptr;
	const std::string service = std::to_string(port);
	if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || !list) {
		throw std::runtime_error("cannot resolve " + host);
	}

	std::vector<SockAddr> out;
	for (struct addrinfo* p = list; p; p = p->ai_next) {
		SockAddr a;
		if (static_cast<std::size_t>(p->ai_addrlen) > sizeof(a.data)) continue;
		std::memcpy(a.data, p->ai_addr, p->ai_addrlen);
		a.len = static_cast<int>(p->ai_addrlen);
		a.family = p->ai_family;
		out.push_back(a);
	}
	freeaddrinfo(list);
	if (out.empty()) throw std::runtime_error("cannot resolve " + host);
	return out;
}

static int bound_port(native_t fd) {
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) return 0;
	if (ss.ss_family == AF_INET) return ntohs(reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port);
	if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_port);
	return 0;
}

Listener listen_tcp(const std::string& host, int port, int backlog) {
	const std::vector<SockAddr> addrs = resolve_tcp(host, port);
	const SockAddr& addr = addrs.front();

	native_t fd = ::socket(addr.family, SOCK_STREAM, 0);
	if (static_cast<socket_t>(fd) == kInvalidSocket) throw std::runtime_error("socket() failed");

	int on = 1;
#ifdef _WIN32
	//Windows の SO_REUSEADDR は他プロセスによるポートの横取りを許すので、排他で取る
	setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

	if (::bind(fd, reinterpret_cast<const struct sockaddr*>(addr.data), addr.len) != 0 || ::listen(fd, backlog) != 0) {
		close_socket(static_cast<socket_t>(fd));
		throw std::runtime_error("cannot listen on " + host + ":" + std::to_string(port));
	}
	set_nonblocking(static_cast<socket_t>(fd));

	Listener l;
	l.fd = static_cast<socket_t>(fd);
	l.port = bound_port(fd);
	return l;
}

socket_t accept_tcp(socket_t listener, std::string* peer) {
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	native_t fd = ::accept(static_cast<native_t>(listener), reinterpret_cast<struct sockaddr*>(&ss), &len);
	if (static_cast<socket_t>(fd) == kInvalidSocket) return kInvalidSocket;

	if (peer) {
		char host[64] = "";
		getnameinfo(reinterpret_cast<struct sockaddr*>(&ss), len, host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
		*peer = host;
	}
	set_nonblocking(static_cast<socket_t>(fd));
	set_nodelay(static_cast<socket_t>(fd));
	return static_cast<socket_t>(fd);
}

socket_t start_connect(const SockAddr& addr, bool* in_progress) {
	net_init();
	*in_progress = false;
	native_t fd = ::socket(addr.family, SOCK_STREAM, 0);
	if (static_cast<socket_t>(fd) == kInvalidSocket) return kInvalidSocket;
	set_nonblocking(static_cast<socket_t>(fd));
	set_nodelay(static_cast<socket_t>(fd));

	if (::connect(fd, reinterpret_cast<const struct sockaddr*>(addr.data), addr.len) == 0) return static_cast<socket_t>(fd);
	if (last_error_in_progress()) {
		*in_progress = true;
		return static_cast<socket_t>(fd);
	}
	close_socket(static_cast<socket_t>(fd));
	return kInvalidSocket;
}

unsigned wait_socket(socket_t fd, unsigned interest, int timeout_ms) {
	pollfd_t p;
	std::memset(&p, 0, sizeof(p));
	p.fd = static_cast<native_t>(fd);
	if (interest & kRead) p.events |= POLLIN;
	if (interest & kWrite) p.events |= POLLOUT;

	const int n = poll_sockets(&p, 1, timeout_ms);
	if (n <= 0) return 0;

	unsigned out = 0;
	if (p.revents & POLLIN) out |= kRead;
	if (p.revents & POLLOUT) out |= kWrite;
	if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) out |= kError | kRead;
	return out;
}

socket_t connect_tcp(const std::string& host, int port, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (const SockAddr& addr : resolve_tcp(host, port)) {
		bool in_progress = false;
		socket_t fd = start_connect(addr, &in_progress);
		if (fd == kInvalidSocket) continue;
		if (!in_progress) return fd;

		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left > 0 && (wait_socket(fd, kWrite, static_cast<int>(left)) & kWrite) && socket_error(fd) == 0) return fd;
		close_socket(fd);
	}
	throw std::runtime_error("cannot connect to " + host + ":" + std::to_string(port));
}

void make_wake_pair(socket_t out[2]) {
	net_init();
#ifdef _WIN32
	//Windows には socketpair が無いので、ループバックの TCP 接続で作る
	Listener l = listen_tcp("127.0.0.1", 0, 1);
	out[1] = connect_tcp("127.0.0.1", l.port, 5000);
	wait_socket(l.fd, kRead, 5000);
	out[0] = accept_tcp(l.fd, nullptr);
	close_socket(l.fd);
	if (out[0] == kInvalidSocket) throw std::runtime_error("cannot create wake pair");
#else
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw std::runtime_error("socketpair failed");
	out[0] = fds[0];
	out[1] = fds[1];
	set_nonblocking(out[0]);
#endif
	set_nonblocking(out[1]);
}

}
