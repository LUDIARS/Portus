#include "portus/push.hpp"

#include "portus/net.hpp"
#include "portus/url.hpp"

namespace portus {

int send_notification(const PushSubscription& sub, std::string_view payload, const VapidKeys& vapid, const SendOptions& options) {
	const std::vector<std::string>& hosts = options.allowed_hosts.empty() ? default_push_hosts() : options.allowed_hosts;
	if (!is_allowed_endpoint(sub.endpoint, hosts)) throw PushError("endpoint is not an allowed push service", 0);

	Url url;
	parse_url(sub.endpoint, url);

	FetchRequest req;
	req.method = "POST";
	req.timeout_ms = options.timeout_ms;
	req.headers = {
		{ "Authorization", create_vapid_authorization(vapid, url.origin()) },
		{ "Content-Encoding", "aes128gcm" },
		{ "Content-Type", "application/octet-stream" },
		{ "TTL", std::to_string(options.ttl_sec) },
	};
	req.body = encrypt_payload(sub.keys, payload);

	const FetchResponse res = fetch(sub.endpoint, req);
	if (!res.ok()) throw PushError("push service responded " + std::to_string(res.status) + ": " + res.body.substr(0, 200), res.status);
	return res.status;
}

}
