#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace portus {

// Identifies a file independently of the name used to open it.
struct FileFollowId {
	std::uint64_t volume;
	// Windows: raw FILE_ID_128. POSIX: inode in little-endian order, zero-extended.
	std::array<unsigned char, 16> file;
	bool operator==(const FileFollowId& other) const noexcept {
		return volume == other.volume && file == other.file;
	}
};

// Metadata captured from the open handle at the start of a scan.
struct FileFollowSnapshot {
	FileFollowId id;
	std::uint64_t size;
};

enum class FileFollowStatus { complete, truncated, line_too_long };

// A batch of complete lines and the checkpoint it proposes to the caller.
struct FileFollowRead {
	std::string bytes; // Raw bytes through the last LF, including line terminators.
	std::uint64_t next_offset;
	FileFollowStatus status;
};

namespace detail { class FollowFile; }

// An open regular file bounded by its opening length. Reopen to observe appends.
class FileFollower {
public:
	// Open the given path for one scan using a read-only handle.
	explicit FileFollower(const std::filesystem::path& path);
	~FileFollower();
	FileFollower(const FileFollower&) = delete;
	FileFollower& operator=(const FileFollower&) = delete;

	// Return the ID and length captured when this reader opened the file.
	[[nodiscard]] FileFollowSnapshot snapshot() const noexcept { return snapshot_; }
	// A successful read proposes an offset for the caller to commit. An empty result
	// and either diagnostic preserve the original offset. byte_limit must be positive.
	// OS errors throw std::system_error. Read and snapshot may be called concurrently.
	[[nodiscard]] FileFollowRead read(std::uint64_t offset, std::size_t byte_limit) const;

private:
	std::unique_ptr<detail::FollowFile> file_;
	FileFollowSnapshot snapshot_;
};

}
