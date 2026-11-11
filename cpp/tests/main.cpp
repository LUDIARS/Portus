#include <cstdio>
#include <cstring>

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
