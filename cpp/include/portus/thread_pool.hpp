#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace portus {

//固定本数のワーカースレッド。ブロックする処理 (ハンドラ) をイベントループの外で動かす
//待ち行列には上限があり、溢れたら submit が false を返す (メモリを際限なく使わない)
class ThreadPool {
public:
	explicit ThreadPool(std::size_t threads, std::size_t max_queue = 1024);
	~ThreadPool();	//受け付け済みの仕事を終えてから合流する。自分のワーカーの中から破棄してもよい (そのワーカーは待たずに切り離す)
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	bool submit(std::function<void()> task);

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

}
