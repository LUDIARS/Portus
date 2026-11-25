#pragma once
#include "augur/contract_runtime.hpp"
#include "portus/file_follow.hpp"
#include <algorithm>
#include <optional>

namespace contracts {
inline const auto follow_window_checks = augur::predicates(
    [](std::uint64_t, std::uint64_t, std::uint64_t, std::size_t limit) { return limit > 0; },
    [](std::optional<std::size_t> result, std::uint64_t snapshot, std::uint64_t current,
       std::uint64_t offset, std::size_t limit) {
        if (offset > snapshot || offset > current) return !result;
        const auto available = std::min(snapshot, current) - offset;
        return result && *result <= limit && *result <= available
            && (*result == limit || *result == available);
    });
}
