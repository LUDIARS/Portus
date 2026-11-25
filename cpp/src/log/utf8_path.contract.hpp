#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

#include "../paging/augur/contract_runtime.hpp"

namespace contracts {
inline const auto utf8_path_checks = augur::predicates(
	[](const std::string&) { return true; },
	[](const std::filesystem::path& result, const std::string& text) {
		const auto bytes = result.u8string();
		return std::equal(bytes.begin(), bytes.end(), text.begin(), text.end(),
			[](auto byte, char expected) {
				return static_cast<unsigned char>(byte) == static_cast<unsigned char>(expected);
			});
	});
}
