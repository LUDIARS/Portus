#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "portus/socket.hpp"

namespace portus {

//単一スレッドのイベントループ (poll)。ソケットの事象・タイマー・他スレッドからの依頼を 1 本の流れで処理する
//  add / set_interest / remove / set_timer / cancel_timer はループスレッド (または run 開始前) から呼ぶ
//  post / stop はどのスレッドからでも呼べる
class EventLoop {
public:
	using IoHandler = std::function<void(unsigned events)>;
	using Task = std::function<void()>;

	EventLoop();
	~EventLoop();
	EventLoop(const EventLoop&) = delete;
	EventLoop& operator=(const EventLoop&) = delete;

	void add(socket_t fd, unsigned interest, IoHandler handler);
	void set_interest(socket_t fd, unsigned interest);
	void remove(socket_t fd);

	std::uint64_t set_timer(int delay_ms, Task task, bool repeat = false);
	void cancel_timer(std::uint64_t id);

	void post(Task task);			//ループスレッドで実行させる
	void dispatch(Task task);		//ループスレッド上 (または未起動) ならその場で、違えば post
	void dispatch_sync(Task task);	//dispatch して完了を待つ

	void run();		//stop が呼ばれるまで戻らない
	void stop();
	bool in_loop_thread() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

//ループを専用スレッドで回す。生成時に起動を待ち、破棄時に止めて合流する
class LoopThread {
public:
	LoopThread();
	~LoopThread();
	EventLoop& loop() { return loop_; }

private:
	EventLoop loop_;
	std::thread thread_;
};

}
