#include "portus/log.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "portus/util.hpp"

namespace portus {

struct AsyncSink::Impl {
	SinkList sinks;
	std::size_t max_queue = 0;

	std::mutex mu;
	std::condition_variable wake;
	std::condition_variable idle;
	std::deque<LogRecord> queue;
	std::size_t dropped = 0;
	bool busy = false;
	int flush_waiters = 0;
	bool closing = false;
	std::thread thread;

	void emit(const LogRecord& rec) {
		for (const auto& sink : sinks) {
			try {
				sink->write(rec);
			} catch (...) {
				//ログ出力の失敗で処理を止めない
			}
		}
	}

	void work() {
		std::unique_lock<std::mutex> lock(mu);
		for (;;) {
			wake.wait(lock, [this] { return closing || !queue.empty(); });
			if (queue.empty()) break;

			std::deque<LogRecord> batch;
			batch.swap(queue);
			const std::size_t lost = dropped;
			dropped = 0;
			busy = true;
			lock.unlock();

			for (const LogRecord& rec : batch) emit(rec);
			if (lost > 0) {
				LogRecord rec;
				rec.time = iso_time(now_ms());
				rec.level = LogLevel::Warn;
				rec.scope = "log";
				rec.msg = "log records dropped";
				rec.fields = Json::object({ { "count", Json(lost) } });
				emit(rec);
			}
			for (const auto& sink : sinks) sink->flush();

			lock.lock();
			busy = false;
			idle.notify_all();
			//続けて来る分をまとめて書く (1 件ごとに起きると、書き込みよりスレッドの切り替えが高くつく)
			if (!closing && flush_waiters == 0) wake.wait_for(lock, std::chrono::milliseconds(2), [this] { return closing || flush_waiters > 0; });
		}
	}
};

AsyncSink::AsyncSink(SinkList sinks, std::size_t max_queue) : impl_(new Impl()) {
	impl_->sinks = std::move(sinks);
	impl_->max_queue = max_queue;
	impl_->thread = std::thread([this] { impl_->work(); });
}

AsyncSink::~AsyncSink() {
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->closing = true;
	}
	impl_->wake.notify_all();
	impl_->thread.join();
}

void AsyncSink::write(const LogRecord& rec) {
	bool was_empty = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->queue.size() >= impl_->max_queue) {
			++impl_->dropped;
			return;
		}
		was_empty = impl_->queue.empty();
		impl_->queue.push_back(rec);
	}
	if (was_empty) impl_->wake.notify_one();
}

void AsyncSink::flush() {
	std::unique_lock<std::mutex> lock(impl_->mu);
	++impl_->flush_waiters;
	impl_->wake.notify_all();
	impl_->idle.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
	--impl_->flush_waiters;
}

}
