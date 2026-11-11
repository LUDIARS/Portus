#include "portus/push.hpp"

#include "portus/url.hpp"
#include "portus/util.hpp"

namespace portus {

const std::vector<std::string>& default_push_hosts() {
	static const std::vector<std::string> hosts = {
		"fcm.googleapis.com",
		"updates.push.services.mozilla.com",
		"web.push.apple.com",
		".push.apple.com",
		".notify.windows.com",
	};
	return hosts;
}

bool is_allowed_endpoint(const std::string& endpoint, const std::vector<std::string>& hosts) {
	Url url;
	if (!parse_url(endpoint, url)) return false;
	if (url.scheme != "https" || !url.userinfo.empty()) return false;

	for (const std::string& h : hosts) {
		if (!h.empty() && h[0] == '.' ? ends_with(url.host, h) : url.host == h) return true;
	}
	return false;
}

}
