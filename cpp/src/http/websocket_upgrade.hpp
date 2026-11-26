#pragma once

#include "portus/http_websocket.hpp"

namespace portus::detail {

enum class UpgradeDisposition { Http, BadRequest, WebSocket };

bool valid_websocket_upgrade_options(const WebSocketUpgradeOptions& options);
UpgradeDisposition inspect_websocket_upgrade(const RequestHead& head, std::string_view path, std::size_t body_length);

//アップグレード済み接続の追跡。実際の接続の寿命は WebSocket と TcpConn が管理する。
class UpgradedWebSockets {
public:
	std::size_t live_count();
	void adopt(EventLoop& loop, const WebSocketUpgradeOptions& options, const std::shared_ptr<TcpConn>& tcp,
		const RequestHead& head, const Json& authorization, std::string_view unread);
	void close();

private:
	std::vector<std::weak_ptr<WebSocket>> sockets_;
};

}
