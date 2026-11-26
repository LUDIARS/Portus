#include "websocket_upgrade.hpp"

#include <algorithm>

#include "portus/tcp_conn.hpp"
#include "augur/contract_runtime.hpp" /* augur-inject:import:2ea1e84a */
#include "websocket_upgrade.contract.hpp" /* augur-inject:contract-predicate:0827e8c4 */

namespace portus {
namespace detail {

bool valid_websocket_upgrade_options(const WebSocketUpgradeOptions& options) { return ::augur::invoke_observed(::augur::ContractSpec{"C-7", "da690722", "cpp/src/http/websocket_upgrade.cpp:10"}, contracts::websocket_upgrade_checks, [&]() -> bool { /* augur-inject:contract-wrap:da690722 */
	if (options.path.empty() || options.path.front() != '/' || !options.authorize || !options.on_connection
		|| options.max_buffered == 0 || options.max_payload == 0) return false;
	return std::none_of(options.path.begin(), options.path.end(), [](unsigned char c) {
		return c <= 0x20 || c == 0x7f || c == '?' || c == '#';
	});
}, options); /* augur-inject:contract-end:da690722 */ }

static bool has_token(const Headers& headers, std::string_view name, std::string_view token) {
	for (const auto& header : headers.all()) {
		if (header.first == name && header_has_token(header.second, token)) return true;
	}
	return false;
}

UpgradeDisposition inspect_websocket_upgrade(const RequestHead& head, std::string_view path, std::size_t body_length) {
	const std::string_view target = head.target;
	if (target.substr(0, target.find('?')) != path || !has_token(head.headers, "upgrade", "websocket")) {
		return UpgradeDisposition::Http;
	}
	if (head.minor_version != 1 || body_length != 0 || head.headers.count("origin") > 1
		|| head.headers.count("sec-websocket-version") != 1 || has_token(head.headers, "connection", "close")
		|| validate_upgrade(head.method, head.headers)) {
		return UpgradeDisposition::BadRequest;
	}
	return UpgradeDisposition::WebSocket;
}

std::size_t UpgradedWebSockets::live_count() {
	sockets_.erase(std::remove_if(sockets_.begin(), sockets_.end(), [](const auto& weak) {
		const auto ws = weak.lock();
		return !ws || ws->ready_state() == WebSocket::CLOSED;
	}), sockets_.end());
	return sockets_.size();
}

void UpgradedWebSockets::adopt(EventLoop& loop, const WebSocketUpgradeOptions& options,
	const std::shared_ptr<TcpConn>& tcp, const RequestHead& head, const Json& authorization, std::string_view unread) {
	const std::string response = build_accept_response(head.headers.get("sec-websocket-key"));
	auto ws = std::make_shared<WebSocket>(loop, false, options.max_payload);
	sockets_.push_back(ws);
	tcp->set_on_drain({});
	tcp->set_max_buffered(options.max_buffered);
	ws->attach(tcp);
	tcp->write(response);
	if (tcp->closed()) return;

	options.on_connection(ws, head, authorization);
	ws->receive(unread);
}

void UpgradedWebSockets::close() {
	std::vector<std::weak_ptr<WebSocket>> sockets;
	sockets.swap(sockets_);
	for (const auto& weak : sockets) {
		if (auto ws = weak.lock()) ws->terminate();
	}
}

}
}
