#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "test.hpp"

namespace testing {

static std::atomic<int> g_failures{0};

std::vector<Case>& registry() {
	static std::vector<Case> cases;
	return cases;
}

void report_failure(const char* file, int line, const std::string& what) {
	++g_failures;
	std::fprintf(stderr, "    FAILED %s:%d  %s\n", file, line, what.c_str());
}

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!cond()) {
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

std::string raw_exchange(int port, const std::string& data, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	const portus::socket_t fd = portus::connect_tcp("127.0.0.1", port, timeout_ms);
	std::size_t sent = 0;
	while (sent < data.size()) {
		const long n = portus::socket_send(fd, data.data() + sent, data.size() - sent);
		if (n > 0) sent += static_cast<std::size_t>(n);
		else if (!portus::last_error_would_block()) break;
		else portus::wait_socket(fd, portus::kWrite, 100);
	}

	std::string out;
	char buf[4096];
	while (std::chrono::steady_clock::now() < deadline) {
		const long n = portus::socket_recv(fd, buf, sizeof(buf));
		if (n > 0) out.append(buf, static_cast<std::size_t>(n));
		else if (n == 0 || !portus::last_error_would_block()) break;
		else portus::wait_socket(fd, portus::kRead, 50);
	}
	portus::close_socket(fd);
	return out;
}

portus::socket_t connect_with_receive_buffer(int port, int bytes, int* granted) {
	//接続の前に決める。受信窓の拡大係数は SYN で決まり、Windows は SO_RCVBUF を明示したソケットだけ受信窓の自動調整を止める。
	//  接続後に変えるのでは、どこまで効くかが OS 次第になる
	portus::net_init();
	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<unsigned short>(port));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int got = 0;
	socklen_t got_len = sizeof(got);
#ifdef _WIN32
	const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) throw std::runtime_error("socket() failed");
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
	if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
		closesocket(fd);
		throw std::runtime_error("connect() failed");
	}
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&got), &got_len);
#else
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) throw std::runtime_error("socket() failed");
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
	if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
		::close(fd);
		throw std::runtime_error("connect() failed");
	}
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &got_len);
#endif
	if (granted) *granted = got;
	const portus::socket_t s = static_cast<portus::socket_t>(fd);
	portus::set_nonblocking(s);
	portus::set_nodelay(s);
	return s;
}

#ifndef _WIN32
FdExhaustion::FdExhaustion(int spare) {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
	saved_cur_ = static_cast<unsigned long long>(rl.rlim_cur);
	int highest = 2;
	const int scan = rl.rlim_cur < 65536 ? static_cast<int>(rl.rlim_cur) : 65536;
	for (int fd = 0; fd < scan; ++fd) {
		if (fcntl(fd, F_GETFD) != -1) highest = fd;
	}
	struct rlimit lowered = rl;
	lowered.rlim_cur = static_cast<rlim_t>(highest + 1 + 8);
	if (setrlimit(RLIMIT_NOFILE, &lowered) != 0) return;
	for (;;) {
		const int fd = dup(0);
		if (fd < 0) break;
		fillers_.push_back(fd);
	}
	for (int i = 0; i < spare && !fillers_.empty(); ++i) {
		close(fillers_.back());
		fillers_.pop_back();
	}
	ok_ = true;
}

FdExhaustion::~FdExhaustion() {
	release();
}

void FdExhaustion::release() {
	if (restored_) return;
	restored_ = true;
	for (int fd : fillers_) close(fd);
	fillers_.clear();
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && saved_cur_) {
		rl.rlim_cur = static_cast<rlim_t>(saved_cur_);
		setrlimit(RLIMIT_NOFILE, &rl);
	}
}

static double cpu_now() {
	struct rusage u;
	getrusage(RUSAGE_SELF, &u);
	return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

double cpu_seconds_during(int ms) {
	const double before = cpu_now();
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	return cpu_now() - before;
}
#endif

}

int main(int argc, char** argv) {
	const char* filter = argc > 1 ? argv[1] : nullptr;
	int ran = 0;
	int failed_cases = 0;
	for (const testing::Case& c : testing::registry()) {
		if (filter && !std::strstr(c.name, filter)) continue;
		const int before = testing::g_failures.load();
		try {
			c.fn();
		} catch (const std::exception& ex) {
			testing::report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + ex.what());
		} catch (...) {
			testing::report_failure(__FILE__, __LINE__, "unexpected exception");
		}
		const bool ok = testing::g_failures.load() == before;
		std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
		std::fflush(stdout);
		++ran;
		if (!ok) ++failed_cases;
	}
	std::printf("\n%d tests, %d failed\n", ran, failed_cases);
	return failed_cases == 0 && ran > 0 ? 0 : 1;
}
