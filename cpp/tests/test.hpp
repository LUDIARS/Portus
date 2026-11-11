#pragma once
//依存を増やさないための最小のテスト枠
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "portus/log.hpp"
#include "portus/socket.hpp"

namespace testing {

struct Case {
	const char* name;
	void (*fn)();
};
std::vector<Case>& registry();
void report_failure(const char* file, int line, const std::string& what);

struct Registrar {
	Registrar(const char* name, void (*fn)()) { registry().push_back(Case{ name, fn }); }
};

//条件が真になるまで待つ
bool wait_until(const std::function<bool()>& cond, int timeout_ms = 3000);

//ループバックの port へ data を送り、相手が閉じるか timeout になるまでに届いたものを返す
std::string raw_exchange(int port, const std::string& data, int timeout_ms = 3000);

//記録をメモリに溜める出力先
class MemorySink : public portus::LogSink {
public:
	void write(const portus::LogRecord& rec) override {
		std::lock_guard<std::mutex> lock(mu_);
		records_.push_back(rec);
	}
	std::vector<portus::LogRecord> records() {
		std::lock_guard<std::mutex> lock(mu_);
		return records_;
	}

private:
	std::mutex mu_;
	std::vector<portus::LogRecord> records_;
};

}

#define TEST(id, description) \
	static void test_##id(); \
	static testing::Registrar registrar_##id(description, test_##id); \
	static void test_##id()

#define CHECK(cond) \
	do { \
		if (!(cond)) testing::report_failure(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		if (!((a) == (b))) testing::report_failure(__FILE__, __LINE__, #a " == " #b); \
	} while (0)

#define CHECK_THROWS(expr) \
	do { \
		bool threw = false; \
		try { \
			(void)(expr); \
		} catch (...) { \
			threw = true; \
		} \
		if (!threw) testing::report_failure(__FILE__, __LINE__, "expected exception: " #expr); \
	} while (0)
