#pragma once
#include "augur/contract_runtime.hpp"
#include "portus/file_follow.hpp"
#include <string_view>

namespace contracts {
inline const auto follow_prefix_checks = augur::predicates(
    [](std::string_view bytes, std::size_t limit, bool) { return bytes.size() <= limit; },
    [](const auto& result, std::string_view bytes, std::size_t limit, bool has_more) {
        const auto end = result.size;
        if (end > bytes.size()) return false;
        if (end > 0) {
            return bytes[end - 1] == '\n' && bytes.find('\n', end) == std::string_view::npos
                && result.status == portus::FileFollowStatus::complete;
        }
        const bool oversized = bytes.size() == limit && has_more;
        return bytes.find('\n') == std::string_view::npos
            && result.status == (oversized ? portus::FileFollowStatus::line_too_long
                                          : portus::FileFollowStatus::complete);
    });
}
