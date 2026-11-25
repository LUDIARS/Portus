#pragma once
#include "augur/contract_runtime.hpp"
#include <cstddef>
#include <limits>

namespace contracts {
inline const auto page_count_checks = augur::predicates(
    [](std::size_t, std::size_t) { return true; },
    [](std::size_t result, std::size_t limit, std::size_t maximum) {
        if (limit == 0 || limit > maximum || limit == std::numeric_limits<std::size_t>::max())
            return result == 0;
        return result > limit && result - limit == 1;
    });
}
