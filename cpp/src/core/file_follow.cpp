#include "portus/file_follow.hpp"
#include "follow_file.hpp"

#include <lorica/checked_size.hpp>
#include <lorica/input_budget.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "augur/contract_runtime.hpp" /* augur-inject:import:916b4297 */
#include "follow_window.contract.hpp" /* augur-inject:contract-predicate:fb6e9426 */
#include "follow_prefix.contract.hpp" /* augur-inject:contract-predicate:13d7113d */

namespace portus {

static std::optional<std::size_t> follow_window(std::uint64_t snapshot_size, std::uint64_t current_size,
	std::uint64_t offset, std::size_t byte_limit) { return ::augur::invoke_observed(::augur::ContractSpec{"C-3", "486298a0", "cpp/src/core/file_follow.cpp:15"}, contracts::follow_window_checks, [&]() -> std::optional<std::size_t> { /* augur-inject:contract-wrap:486298a0 */
	const auto end = std::min(snapshot_size, current_size);
	if (offset > end) return std::nullopt;
	return static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, byte_limit));
}, snapshot_size, current_size, offset, byte_limit); /* augur-inject:contract-end:486298a0 */ }

struct FollowPrefix {
	std::size_t size;
	FileFollowStatus status;
};

static FollowPrefix follow_prefix(std::string_view bytes, std::size_t byte_limit, bool has_more) { return ::augur::invoke_observed(::augur::ContractSpec{"C-4", "813773fb", "cpp/src/core/file_follow.cpp:27"}, contracts::follow_prefix_checks, [&]() -> FollowPrefix { /* augur-inject:contract-wrap:813773fb */
	const auto newline = bytes.rfind('\n');
	if (newline != std::string_view::npos) return {newline + 1, FileFollowStatus::complete};
	if (bytes.size() == byte_limit && has_more) return {0, FileFollowStatus::line_too_long};
	return {0, FileFollowStatus::complete};
}, bytes, byte_limit, has_more); /* augur-inject:contract-end:813773fb */ }

FileFollower::FileFollower(const std::filesystem::path& path)
	: file_(std::make_unique<detail::FollowFile>(path)), snapshot_(file_->snapshot()) {}

FileFollower::~FileFollower() = default;

// Recheck length after I/O so a concurrent shrink cannot commit a stale cursor.
FileFollowRead FileFollower::read(std::uint64_t offset, std::size_t byte_limit) const {
	if (byte_limit == 0) throw std::invalid_argument("FileFollower byte limit must be positive");
	const auto window = follow_window(snapshot_.size, file_->size(), offset, byte_limit);
	if (!window) return {{}, offset, FileFollowStatus::truncated};
	if (*window == 0) return {{}, offset, FileFollowStatus::complete};

	lorica::input_budget budget({byte_limit, 0, 1, 0});
	if (!budget.consume(*window, 0, 1)) throw std::length_error("FileFollower input budget exceeded");
	std::string bytes(*window, '\0');
	std::size_t read_bytes = 0;
	while (read_bytes < bytes.size()) {
		const auto destination = std::span<char>(bytes).subspan(read_bytes);
		const auto count = file_->read_at(offset + read_bytes, destination);
		if (count == 0) break;
		const auto next = lorica::checked_add(read_bytes, count);
		if (!next || *next > bytes.size()) throw std::length_error("FileFollower received an invalid read count");
		read_bytes = *next;
	}

	const auto end = std::min(snapshot_.size, file_->size());
	if (end < offset) return {{}, offset, FileFollowStatus::truncated};
	bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, read_bytes)));
	const auto prefix = follow_prefix(bytes, byte_limit, end - offset > bytes.size());
	bytes.resize(prefix.size);
	return {std::move(bytes), offset + prefix.size, prefix.status};
}

}
