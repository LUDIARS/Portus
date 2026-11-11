#include "portus/thread_pool.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace portus {

struct ThreadPool::Impl {
	std::mutex mu;
	std::condition_variable cv;
	std::deque<std::function<void()>> queue;
	std::vector<std::thread> threads;
	std::size_t max_queue = 0;
	bool closing = false;

	void work() {
		for (;;) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mu);
				cv.wait(lock, [this] { return closing || !queue.empty(); });
				if (queue.empty()) return;
				task = std::move(queue.front());
				queue.pop_front();
			}
			task();
		}
	}
};

ThreadPool::ThreadPool(std::size_t threads, std::size_t max_queue) : impl_(new Impl()) {
	impl_->max_queue = max_queue;
	if (threads == 0) threads = 1;
	for (std::size_t i = 0; i < threads; ++i) impl_->threads.emplace_back([this] { impl_->work(); });
}

ThreadPool::~ThreadPool() {
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->closing = true;
	}
	impl_->cv.notify_all();
	for (std::thread& t : impl_->threads) t.join();
}

bool ThreadPool::submit(std::function<void()> task) {
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		if (impl_->closing || impl_->queue.size() >= impl_->max_queue) return false;
		impl_->queue.push_back(std::move(task));
	}
	impl_->cv.notify_one();
	return true;
}

}
