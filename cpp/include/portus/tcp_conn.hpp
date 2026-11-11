#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "portus/event_loop.hpp"

namespace portus {

//接続済みソケット 1 本の非ブロッキング入出力。全メソッドはループスレッドから呼ぶ
//  送信待ちが max_buffered を超えたら (遅い相手にメモリを取られないよう) 切断する
class TcpConn : public std::enable_shared_from_this<TcpConn> {
public:
	using DataFn = std::function<void(const char* data, std::size_t len)>;
	using CloseFn = std::function<void()>;

	TcpConn(EventLoop& loop, socket_t fd, std::size_t max_buffered);
	~TcpConn();
	TcpConn(const TcpConn&) = delete;
	TcpConn& operator=(const TcpConn&) = delete;

	void set_on_data(DataFn fn);	//実行中のコールバックから差し替えてもよい
	void set_on_close(CloseFn fn);	//閉じたときに 1 回だけ呼ばれる

	void start();
	void write(const std::string& data);
	void pause_read(bool paused);
	void end();		//送り切ってから送信側を閉じ、相手が閉じるのを待つ (以後の受信は捨てる)
	void close();	//すぐ閉じる

	bool closed() const { return fd_ == kInvalidSocket; }
	std::size_t buffered() const { return out_.size() - out_pos_; }

private:
	void handle_io(unsigned events);
	void flush();
	void update_interest();

	EventLoop& loop_;
	socket_t fd_;
	std::size_t max_buffered_;
	std::shared_ptr<DataFn> on_data_;
	std::shared_ptr<CloseFn> on_close_;
	std::string out_;
	std::size_t out_pos_ = 0;
	bool reading_ = true;
	bool ending_ = false;
	bool write_shut_ = false;
	bool peer_eof_ = false;
};

}
