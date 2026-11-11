#include "portus/tcp_conn.hpp"

namespace portus {

TcpConn::TcpConn(EventLoop& loop, socket_t fd, std::size_t max_buffered) : loop_(loop), fd_(fd), max_buffered_(max_buffered) {}

TcpConn::~TcpConn() {
	if (fd_ != kInvalidSocket) close_socket(fd_);
}

void TcpConn::set_on_data(DataFn fn) {
	on_data_ = std::make_shared<DataFn>(std::move(fn));
}

void TcpConn::set_on_close(CloseFn fn) {
	on_close_ = std::make_shared<CloseFn>(std::move(fn));
}

void TcpConn::start() {
	std::shared_ptr<TcpConn> self = shared_from_this();
	loop_.add(fd_, kRead, [self](unsigned events) { self->handle_io(events); });
}

void TcpConn::update_interest() {
	if (closed()) return;
	loop_.set_interest(fd_, (reading_ ? kRead : 0u) | (buffered() > 0 ? kWrite : 0u));
}

void TcpConn::pause_read(bool paused) {
	if (ending_ || peer_eof_) return;
	reading_ = !paused;
	update_interest();
}

void TcpConn::write(const std::string& data) {
	std::shared_ptr<TcpConn> keep = shared_from_this();	//途中で閉じて持ち主に手放されても、戻るまで生かす
	if (closed() || write_shut_ || data.empty()) return;
	if (out_pos_ > 0 && out_pos_ == out_.size()) {
		out_.clear();
		out_pos_ = 0;
	}
	out_.append(data);
	flush();
	if (!closed() && buffered() > max_buffered_) close();
}

void TcpConn::flush() {
	while (buffered() > 0) {
		const long n = socket_send(fd_, out_.data() + out_pos_, buffered());
		if (n > 0) {
			out_pos_ += static_cast<std::size_t>(n);
			continue;
		}
		if (last_error_would_block()) break;
		close();
		return;
	}
	if (buffered() == 0) {
		out_.clear();
		out_pos_ = 0;
		if (peer_eof_) {
			close();
			return;
		}
		if (ending_ && !write_shut_) {
			write_shut_ = true;
			shutdown_write(fd_);
		}
	}
	update_interest();
}

void TcpConn::end() {
	std::shared_ptr<TcpConn> keep = shared_from_this();
	if (closed() || ending_) return;
	ending_ = true;
	reading_ = true;
	flush();
}

void TcpConn::close() {
	if (closed()) return;
	std::shared_ptr<TcpConn> keep = shared_from_this();
	loop_.remove(fd_);
	close_socket(fd_);
	fd_ = kInvalidSocket;
	out_.clear();
	out_pos_ = 0;

	std::shared_ptr<CloseFn> cb = std::move(on_close_);
	on_close_.reset();
	on_data_.reset();
	if (cb && *cb) (*cb)();
}

void TcpConn::handle_io(unsigned events) {
	std::shared_ptr<TcpConn> self = shared_from_this();
	if (closed()) return;

	if (events & kWrite) {
		flush();
		if (closed()) return;
	}
	if (!(events & (kRead | kError))) return;
	if (!reading_) {
		//読み止め中に来るのはエラーか切断だけ
		if (events & kError) close();
		return;
	}

	char buf[65536];
	const long n = socket_recv(fd_, buf, sizeof(buf));
	if (n > 0) {
		if (ending_) return;
		std::shared_ptr<DataFn> cb = on_data_;
		if (cb && *cb) (*cb)(buf, static_cast<std::size_t>(n));
		return;
	}
	if (n < 0 && last_error_would_block()) return;
	if (n == 0 && buffered() > 0) {
		//相手は送信側だけ閉じた。残りの応答を送り切ってから閉じる
		peer_eof_ = true;
		reading_ = false;
		update_interest();
		return;
	}
	close();
}

}
