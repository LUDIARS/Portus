#pragma once

#include <cstdint>
#include <string_view>

#include "augur/contract_runtime.hpp"

namespace contracts {
inline const auto expiration_checks = augur::predicates(
	[](std::string_view) { return true; },
	[](std::int64_t result, std::string_view text) {
		if (text == "2026-01-02T03:04:05Z") return result == 1767323045000LL;
		if (text == "2000-02-29T00:00:00Z") return result == 951782400000LL;
		if (text == "1900-02-29T00:00:00Z" || text.size() < 19) return result == 0;
		return result % 1000 == 0;
	});
}
