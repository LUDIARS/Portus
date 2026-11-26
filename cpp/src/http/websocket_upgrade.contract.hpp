#pragma once

#include "augur/contract_runtime.hpp"
#include "websocket_upgrade.hpp"

namespace contracts {
inline const auto websocket_upgrade_checks = augur::predicates(
	[](const portus::WebSocketUpgradeOptions&) { return true; },
	[](bool result, const portus::WebSocketUpgradeOptions& options) {
		for (const unsigned char c : options.path) {
			if (c <= 0x20 || c == 0x7f || c == '?' || c == '#') return !result;
		}
		const bool complete = !options.path.empty() && options.path.front() == '/' && options.authorize
			&& options.on_connection && options.max_buffered > 0 && options.max_payload > 0;
		return result == complete;
	});
}
