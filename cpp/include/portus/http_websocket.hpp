#pragma once

#include "portus/json.hpp"
#include "portus/ws.hpp"

namespace portus {

//HttpServer の待ち受けで、1 つの明示的なパスを WebSocket に切り替える。
//両コールバックはイベントループ上で同期的に呼ぶ。ブロックする処理は置かないこと。
struct WebSocketUpgradeOptions {
	std::string path = "/ws";
	std::size_t max_payload = kDefaultMaxPayload;
	std::size_t max_buffered = kDefaultMaxBuffered;

	//nullopt は 403。認可結果は 101 の送信後に on_connection へ渡す。公開接続には Json{} を返せる。
	std::function<std::optional<Json>(const RequestHead& request, std::string_view remote_addr)> authorize;
	std::function<void(const std::shared_ptr<WebSocket>& ws, const RequestHead& request,
		const Json& authorization)> on_connection;
};

}
