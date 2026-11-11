#include "portus/push.hpp"

#include <stdexcept>

#include "portus/crypto.hpp"

namespace portus {

namespace {

const std::size_t kRecordSize = 4096;
const std::size_t kTagLength = 16;

//HKDF の info は末尾に NUL を含む
std::string label(const char* text) {
	std::string out(text);
	out.push_back('\0');
	return out;
}

}

std::string encrypt_payload(const SubscriptionKeys& keys, std::string_view payload, const EncryptOptions& options) {
	std::string ua_public;
	std::string auth_secret;
	if (!base64_decode(keys.p256dh, ua_public) || ua_public.size() != 65 || ua_public[0] != 0x04) throw std::invalid_argument("p256dh must be 65-byte uncompressed P-256");
	if (!base64_decode(keys.auth, auth_secret) || auth_secret.size() != 16) throw std::invalid_argument("auth must be 16 bytes");
	if (payload.size() + 1 + kTagLength > kRecordSize) throw std::invalid_argument("payload too large for a single record");

	P256KeyPair server;
	if (options.server_private_key.empty()) {
		server = p256_generate();
	} else {
		server.private_key = options.server_private_key;
		server.public_key = p256_public_from_private(server.private_key);
	}
	const std::string shared = p256_ecdh(server, ua_public);

	const std::string ikm = hkdf_sha256(auth_secret, shared, label("WebPush: info") + ua_public + server.public_key, 32);
	const std::string salt = options.salt.empty() ? random_bytes(16) : options.salt;
	if (salt.size() != 16) throw std::invalid_argument("salt must be 16 bytes");
	const std::string cek = hkdf_sha256(salt, ikm, label("Content-Encoding: aes128gcm"), 16);
	const std::string nonce = hkdf_sha256(salt, ikm, label("Content-Encoding: nonce"), 12);

	//最終レコードの区切り 0x02 を付けて暗号化する
	std::string plain(payload);
	plain.push_back('\x02');
	const std::string encrypted = aes128gcm_encrypt(cek, nonce, plain);

	//ヘッダ: salt(16) | レコード長(4, BE) | 鍵 ID 長(1) | 鍵 ID (= サーバ公開鍵)
	std::string out = salt;
	out.push_back(static_cast<char>((kRecordSize >> 24) & 0xFF));
	out.push_back(static_cast<char>((kRecordSize >> 16) & 0xFF));
	out.push_back(static_cast<char>((kRecordSize >> 8) & 0xFF));
	out.push_back(static_cast<char>(kRecordSize & 0xFF));
	out.push_back(static_cast<char>(server.public_key.size()));
	out += server.public_key;
	out += encrypted;
	return out;
}

}
