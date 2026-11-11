#include "portus/event_loop.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "platform.hpp"

namespace portus {

using Clock = std::chrono::steady_clock;

struct EventLoop::Impl {
	struct Entry {
		std::uint64_t id;
		unsigned interest;
		std::shared_ptr<IoHandler> handler;
	};
	struct Timer {
		std::uint64_t id;
		int repeat_ms;
		Task task;
		bool cancelled = false;
	};

	std::unordered_map<socket_t, Entry> entries;
	std::multimap<Clock::time_point, Timer> timers;
	std::vector<Timer> due;	//今まさに実行中の期限切れタイマー。同じ回の前のタスクが取り消したものは呼ばない
	std::uint64_t next_id = 1;

	std::mutex mu;
	std::vector<Task> posted;
	socket_t wake[2] = { kInvalidSocket, kInvalidSocket };

	std::atomic<bool> stopping{false};
	std::atomic<bool> running{false};	//書き換えは mu の中で。post するか自分で実行するかの判断と食い違わせない
	std::atomic<std::thread::id> thread{};

	void run_posted() {
		std::vector<Task> tasks;
		{
			std::lock_guard<std::mutex> lock(mu);
			tasks.swap(posted);
		}
		for (Task& t : tasks) t();
	}

	//ループが回っていれば積んで true。止まっていれば false (呼び出し側がその場で実行する)
	bool post_if_running(Task& task) {
		bool wake = false;
		{
			std::lock_guard<std::mutex> lock(mu);
			if (!running.load()) return false;
			wake = posted.empty();
			posted.push_back(std::move(task));
		}
		if (wake) socket_send(this->wake[1], "w", 1);
		return true;
	}

	int poll_timeout() {
		if (timers.empty()) return 1000;
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(timers.begin()->first - Clock::now()).count();
		if (left <= 0) return 0;
		return left > 1000 ? 1000 : static_cast<int>(left) + 1;
	}

	void run_timers() {
		const auto now = Clock::now();
		due.clear();
		while (!timers.empty() && timers.begin()->first <= now) {
			due.push_back(std::move(timers.begin()->second));
			timers.erase(timers.begin());
		}
		//タスクの中で set_timer されても due は伸びない (timers に入る) ので添字で回してよい
		for (std::size_t i = 0; i < due.size(); ++i) {
			if (due[i].cancelled) continue;
			if (due[i].repeat_ms > 0) timers.emplace(now + std::chrono::milliseconds(due[i].repeat_ms), due[i]);
			due[i].task();
		}
		due.clear();
	}
};

EventLoop::EventLoop() : impl_(new Impl()) {
	make_wake_pair(impl_->wake);
}

EventLoop::~EventLoop() {
	close_socket(impl_->wake[0]);
	close_socket(impl_->wake[1]);
}

void EventLoop::add(socket_t fd, unsigned interest, IoHandler handler) {
	impl_->entries[fd] = Impl::Entry{ impl_->next_id++, interest, std::make_shared<IoHandler>(std::move(handler)) };
}

void EventLoop::set_interest(socket_t fd, unsigned interest) {
	auto it = impl_->entries.find(fd);
	if (it != impl_->entries.end()) it->second.interest = interest;
}

void EventLoop::remove(socket_t fd) {
	impl_->entries.erase(fd);
}

std::uint64_t EventLoop::set_timer(int delay_ms, Task task, bool repeat) {
	const std::uint64_t id = impl_->next_id++;
	impl_->timers.emplace(Clock::now() + std::chrono::milliseconds(delay_ms), Impl::Timer{ id, repeat ? delay_ms : 0, std::move(task) });
	return id;
}

void EventLoop::cancel_timer(std::uint64_t id) {
	for (Impl::Timer& t : impl_->due) {
		if (t.id == id) t.cancelled = true;
	}
	for (auto it = impl_->timers.begin(); it != impl_->timers.end(); ++it) {
		if (it->second.id == id) {
			impl_->timers.erase(it);
			return;
		}
	}
}

void EventLoop::post(Task task) {
	bool wake = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		wake = impl_->posted.empty();
		impl_->posted.push_back(std::move(task));
	}
	if (wake) socket_send(impl_->wake[1], "w", 1);
}

bool EventLoop::in_loop_thread() const {
	return impl_->running.load() && impl_->thread.load() == std::this_thread::get_id();
}

void EventLoop::dispatch(Task task) {
	if (in_loop_thread() || !impl_->post_if_running(task)) task();
}

void EventLoop::dispatch_sync(Task task) {
	if (in_loop_thread()) {
		task();
		return;
	}
	std::promise<void> done;
	Task wrapped = [&] {
		task();
		done.set_value();
	};
	//止まる直前に積んで取り残されると永久に待つ。積めなかったらここで実行する
	if (!impl_->post_if_running(wrapped)) {
		task();
		return;
	}
	done.get_future().wait();
}

void EventLoop::stop() {
	impl_->stopping.store(true);
	socket_send(impl_->wake[1], "w", 1);
}

void EventLoop::run() {
	Impl& s = *impl_;
	s.thread.store(std::this_thread::get_id());
	{
		std::lock_guard<std::mutex> lock(s.mu);
		s.running.store(true);
	}

	struct Slot {
		socket_t fd;
		std::uint64_t id;
	};
	std::vector<pollfd_t> fds;
	std::vector<Slot> slots;

	while (!s.stopping.load()) {
		s.run_posted();

		fds.clear();
		slots.clear();
		pollfd_t wake_fd{};
		wake_fd.fd = s.wake[0];
		wake_fd.events = POLLIN;
		fds.push_back(wake_fd);
		for (const auto& kv : s.entries) {
			pollfd_t p{};
			p.fd = kv.first;
			if (kv.second.interest & kRead) p.events |= POLLIN;
			if (kv.second.interest & kWrite) p.events |= POLLOUT;
			fds.push_back(p);
			slots.push_back(Slot{ kv.first, kv.second.id });
		}

		const int ready = poll_sockets(fds.data(), static_cast<unsigned long>(fds.size()), s.poll_timeout());

		if (ready > 0) {
			if (fds[0].revents) {
				char buf[256];
				while (socket_recv(s.wake[0], buf, sizeof(buf)) > 0) {}
			}
			for (std::size_t i = 0; i < slots.size(); ++i) {
				const short re = fds[i + 1].revents;
				if (!re) continue;

				//前のハンドラが消したり、同じ番号で作り直したソケットには渡さない
				auto it = s.entries.find(slots[i].fd);
				if (it == s.entries.end() || it->second.id != slots[i].id) continue;

				unsigned events = 0;
				if (re & POLLIN) events |= kRead;
				if (re & POLLOUT) events |= kWrite;
				if (re & (POLLERR | POLLHUP | POLLNVAL)) events |= kError;

				std::shared_ptr<IoHandler> handler = it->second.handler;
				(*handler)(events);
			}
		}

		s.run_timers();
	}

	//running を倒してから最後に流す。倒した後の dispatch は呼び出し側で実行されるので、積まれたまま残るものは無い
	{
		std::lock_guard<std::mutex> lock(s.mu);
		s.running.store(false);
	}
	s.run_posted();
	s.stopping.store(false);
}

LoopThread::LoopThread() {
	std::promise<void> ready;
	loop_.post([&] { ready.set_value(); });
	thread_ = std::thread([this] { loop_.run(); });
	ready.get_future().wait();
}

LoopThread::~LoopThread() {
	loop_.stop();
	if (thread_.joinable()) thread_.join();
}

}
