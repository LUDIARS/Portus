#pragma once

#include "portus/file_follow.hpp"

#include <span>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <mutex>
#endif

namespace portus::detail {

// Owns one platform file handle and translates metadata and positional reads.
class FollowFile {
public:
	// Acquire a native handle, which remains owned until destruction.
	explicit FollowFile(const std::filesystem::path& path);
	~FollowFile();
	FollowFile(const FollowFile&) = delete;
	FollowFile& operator=(const FollowFile&) = delete;

	// Query the native identity and current length on the same handle.
	[[nodiscard]] FileFollowSnapshot snapshot() const;
	// Refresh the length to detect truncation of the opened file.
	[[nodiscard]] std::uint64_t size() const;
	// Read at an offset bounded by the measured length into a nonempty buffer.
	[[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<char> destination) const;

private:
#ifdef _WIN32
	HANDLE handle_;
	mutable std::mutex position_mutex_;
#else
	int fd_;
#endif
};

}
