#include "portus/push.hpp"

#include <stdexcept>

#include "portus/crypto.hpp"
#include "portus/json.hpp"
#include "portus/util.hpp"

namespace portus {

std::string create_vapid_authorization(const VapidKeys& keys, const std::string& audience, long long ttl_sec, long long now) {
	if (!starts_with(keys.subject, "mailto:") && !starts_with(keys.subject, "https:")) throw std::invalid_argument("VAPID subject must be mailto: or https:");

	P256KeyPair pair;
	if (!base64_decode(keys.public_key, pair.public_key) || pair.public_key.size() != 65 || pair.public_key[0] != 0x04) {
		throw std::invalid_argument("VAPID public key must be 65-byte uncompressed P-256");
	}
	if (!base64_decode(keys.private_key, pair.private_key) || pair.private_key.size() != 32) throw std::invalid_argument("VAPID private key must be 32 bytes");

	if (now < 0) now = now_ms();
	const std::string header = base64url_encode(Json::object({ { "typ", Json("JWT") }, { "alg", Json("ES256") } }).dump());
	const std::string claims = base64url_encode(Json::object({ { "aud", Json(audience) }, { "exp", Json(now / 1000 + ttl_sec) }, { "sub", Json(keys.subject) } }).dump());
	const std::string input = header + "." + claims;

	return "vapid t=" + input + "." + base64url_encode(p256_sign_sha256(pair, input)) + ", k=" + keys.public_key;
}

}
