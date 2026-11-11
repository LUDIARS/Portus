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

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PORTUS_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PORTUS_TEST_SANITIZED 1
#endif
#endif

namespace testing {

//計算量を時間で確かめるテストの倍率。サニタイザ付きのビルドは数倍遅い (2 乗時間ならそれでも桁違いに超える)
#ifdef PORTUS_TEST_SANITIZED
constexpr int kTimeScale = 5;
#else
constexpr int kTimeScale = 1;
#endif

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

//受信バッファを固定する (OS の自動拡張で、読まない相手の分まで OS に吸い込まれないように)
void set_receive_buffer(portus::socket_t fd, int bytes);

#ifndef _WIN32
//プロセスの fd を使い切った状態を作る (accept が EMFILE で失敗する状況)。破棄で fd と上限を元に戻す
//  上限を下げ、空いた番号を dup で埋める。spare 本だけ空けておき、呼び出し側が接続などに使う
class FdExhaustion {
public:
	explicit FdExhaustion(int spare);
	~FdExhaustion();
	bool ok() const { return ok_; }
	void release();	//期限前に戻す

private:
	bool ok_ = false;
	bool restored_ = false;
	unsigned long long saved_cur_ = 0;
	std::vector<int> fillers_;
};

//このプロセスが ms の間に使った CPU 時間 (秒)
double cpu_seconds_during(int ms);
#endif

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
