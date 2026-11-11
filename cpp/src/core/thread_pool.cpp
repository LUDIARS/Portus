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
	//ワーカーは Impl を共有で持つ。自分のワーカーの中で破棄されても、そのワーカーは仕事を終えるまで Impl を使える
	std::shared_ptr<Impl> impl = impl_;
	for (std::size_t i = 0; i < threads; ++i) impl_->threads.emplace_back([impl] { impl->work(); });
}

ThreadPool::~ThreadPool() {
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		impl_->closing = true;
	}
	impl_->cv.notify_all();
	//自分のワーカーから破棄されたとき (ハンドラが close したなど)、自分は待てないので切り離す
	for (std::thread& t : impl_->threads) {
		if (t.get_id() == std::this_thread::get_id()) t.detach();
		else t.join();
	}
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
