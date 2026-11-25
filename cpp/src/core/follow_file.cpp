#include "follow_file.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace portus::detail {
namespace {

const std::filesystem::path& validate_path(const std::filesystem::path& path) {
	if (path.empty() || path.native().find(std::filesystem::path::value_type{}) != path.native().npos) {
		throw std::invalid_argument("FileFollower requires a nonempty path without NUL bytes");
	}
	return path;
}

#ifdef _WIN32
[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
	throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

HANDLE open_file(const std::filesystem::path& path) {
	const HANDLE handle = CreateFileW(validate_path(path).c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) fail("CreateFileW");
	return handle;
}
#else
[[noreturn]] void fail(const char* operation) {
	throw std::system_error(errno, std::generic_category(), operation);
}

int open_file(const std::filesystem::path& path) {
	const auto& validated = validate_path(path);
	int fd = -1;
	do {
		// Opening a FIFO must not wait for a writer before we can reject it.
		fd = ::open(validated.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0) fail("open");
	return fd;
}

struct stat file_stat(int fd) {
	struct stat info{};
	int result = -1;
	do {
		result = fstat(fd, &info);
	} while (result < 0 && errno == EINTR);
	if (result < 0) fail("fstat");
	return info;
}
#endif

[[noreturn]] void reject_non_file() {
	throw std::system_error(std::make_error_code(std::errc::invalid_argument), "FileFollower requires a regular file");
}

}

#ifdef _WIN32
FollowFile::FollowFile(const std::filesystem::path& path) : handle_(open_file(path)) {}
FollowFile::~FollowFile() { CloseHandle(handle_); }

FileFollowSnapshot FollowFile::snapshot() const {
	if (GetFileType(handle_) != FILE_TYPE_DISK) reject_non_file();
	FILE_ID_INFO info{};
	if (!GetFileInformationByHandleEx(handle_, FileIdInfo, &info, sizeof(info))) {
		fail("GetFileInformationByHandleEx(FileIdInfo)");
	}
	FileFollowId id{info.VolumeSerialNumber, {}};
	std::copy_n(info.FileId.Identifier, id.file.size(), id.file.begin());
	return {id, size()};
}

std::uint64_t FollowFile::size() const {
	LARGE_INTEGER length{};
	if (!GetFileSizeEx(handle_, &length)) fail("GetFileSizeEx");
	return static_cast<std::uint64_t>(length.QuadPart);
}

std::size_t FollowFile::read_at(std::uint64_t offset, std::span<char> destination) const {
	const std::lock_guard lock(position_mutex_);
	LARGE_INTEGER position{};
	position.QuadPart = static_cast<LONGLONG>(offset);
	if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) fail("SetFilePointerEx");
	const auto length = static_cast<DWORD>(std::min<std::size_t>(destination.size(), MAXDWORD));
	DWORD count = 0;
	if (!ReadFile(handle_, destination.data(), length, &count, nullptr)) {
		const auto error = GetLastError();
		if (error == ERROR_HANDLE_EOF) return 0;
		fail("ReadFile", error);
	}
	return count;
}
#else
FollowFile::FollowFile(const std::filesystem::path& path) : fd_(open_file(path)) {}
FollowFile::~FollowFile() { ::close(fd_); }

FileFollowSnapshot FollowFile::snapshot() const {
	const auto info = file_stat(fd_);
	if (!S_ISREG(info.st_mode)) reject_non_file();
	FileFollowId id{static_cast<std::uint64_t>(info.st_dev), {}};
	std::uint64_t inode = static_cast<std::uint64_t>(info.st_ino);
	for (std::size_t index = 0; index < sizeof(inode); ++index) {
		id.file[index] = static_cast<unsigned char>(inode & 0xff);
		inode >>= 8;
	}
	return {id, static_cast<std::uint64_t>(info.st_size)};
}

std::uint64_t FollowFile::size() const {
	return static_cast<std::uint64_t>(file_stat(fd_).st_size);
}

std::size_t FollowFile::read_at(std::uint64_t offset, std::span<char> destination) const {
	const auto length = std::min(destination.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
	ssize_t count = -1;
	do {
		count = pread(fd_, destination.data(), length, static_cast<off_t>(offset));
	} while (count < 0 && errno == EINTR);
	if (count < 0) fail("pread");
	return static_cast<std::size_t>(count);
}
#endif

}
