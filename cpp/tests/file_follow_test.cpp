#include "test.hpp"
#include "portus/file_follow.hpp"
#include "portus/id.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

using portus::FileFollower;
using portus::FileFollowStatus;
namespace fs = std::filesystem;

namespace {

class FollowFixture {
public:
	FollowFixture() : directory(fs::temp_directory_path() / ("portus-follow-" + portus::uuidv4())) {
		if (!fs::create_directory(directory)) throw std::runtime_error("temporary directory already exists");
	}
	~FollowFixture() {
		std::error_code ignored;
		fs::remove_all(directory, ignored);
	}
	FollowFixture(const FollowFixture&) = delete;
	FollowFixture& operator=(const FollowFixture&) = delete;
	const fs::path directory;
};

void write_bytes(const fs::path& path, std::string_view bytes, std::ios::openmode mode = std::ios::trunc) {
	std::ofstream stream;
	stream.exceptions(std::ios::failbit | std::ios::badbit);
	stream.open(path, std::ios::binary | mode);
	stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	stream.close();
}

}

TEST(file_follow_identity, "file follow: rename, hard link, and replacement preserve handle identity") {
	const FollowFixture fixture;
	const auto original = fixture.directory / "live.log";
	const auto archive = fixture.directory / "archive.log";
	const auto link = fixture.directory / "link.log";
	write_bytes(original, "old\n");
	const FileFollower opened(original);
	fs::rename(original, archive);
	fs::create_hard_link(archive, link);
	write_bytes(original, "new\n");
	CHECK(opened.snapshot().id == FileFollower(archive).snapshot().id);
	CHECK(opened.snapshot().id == FileFollower(link).snapshot().id);
	CHECK(!(opened.snapshot().id == FileFollower(original).snapshot().id));
	CHECK_EQ(opened.snapshot().size, 4U);
	CHECK_EQ(opened.read(0, 16).bytes, "old\n");
}

TEST(file_follow_snapshot, "file follow: byte cursors and complete lines use the opening length") {
	const FollowFixture fixture;
	const auto path = fixture.directory / "live.log";
	write_bytes(path, "a\r\n\nb");
	const FileFollower opened(path);
	write_bytes(path, "\n", std::ios::app);
	const auto first = opened.read(0, 3);
	CHECK_EQ(first.bytes, "a\r\n");
	CHECK_EQ(first.next_offset, 3U);
	const auto second = opened.read(first.next_offset, 16);
	CHECK_EQ(second.bytes, "\n");
	CHECK_EQ(second.next_offset, 4U);
	const auto pending = opened.read(second.next_offset, 16);
	CHECK(pending.bytes.empty());
	CHECK_EQ(pending.next_offset, 4U);
	CHECK_EQ(pending.status, FileFollowStatus::complete);
	const auto resumed = FileFollower(path).read(pending.next_offset, 16);
	CHECK_EQ(resumed.bytes, "b\n");
	CHECK_EQ(resumed.next_offset, 6U);
}

TEST(file_follow_limit, "file follow: oversized lines preserve the cursor and an exact limit includes LF") {
	const FollowFixture fixture;
	const auto path = fixture.directory / "live.log";
	write_bytes(path, "a\n1234\n");
	const FileFollower opened(path);
	const auto prefix = opened.read(0, 4);
	CHECK_EQ(prefix.bytes, "a\n");
	CHECK_EQ(prefix.next_offset, 2U);
	const auto oversized = opened.read(prefix.next_offset, 4);
	CHECK_EQ(oversized.status, FileFollowStatus::line_too_long);
	CHECK(oversized.bytes.empty());
	CHECK_EQ(oversized.next_offset, 2U);
	CHECK_EQ(opened.read(2, 5).bytes, "1234\n");
	CHECK_THROWS(opened.read(0, 0));
}

TEST(file_follow_truncation, "file follow: truncation is visible before and after opening") {
	const FollowFixture fixture;
	const auto path = fixture.directory / "live.log";
	write_bytes(path, "a\nb\n");
	const FileFollower opened(path);
	fs::resize_file(path, 2);
	const auto truncated = {opened.read(4, 8), FileFollower(path).read(4, 8)};
	for (const auto& result : truncated) {
		CHECK_EQ(result.status, FileFollowStatus::truncated);
		CHECK(result.bytes.empty());
		CHECK_EQ(result.next_offset, 4U);
	}
	CHECK_EQ(opened.read(2, 8).status, FileFollowStatus::complete);
	CHECK_EQ(opened.read(0, 8).bytes, "a\n");
}
