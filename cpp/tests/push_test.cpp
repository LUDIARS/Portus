#include <openssl/evp.h>

#include "portus/crypto.hpp"
#include "portus/json.hpp"
#include "portus/push.hpp"
#include "test.hpp"

using namespace portus;

namespace {

//RFC 8291 Appendix A の検証値
const char* const kPlaintext = "When I grow up, I want to be a watermelon";
const char* const kAsPrivate = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
const char* const kUaPrivate = "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94";
const char* const kUaPublic = "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
const char* const kAuthSecret = "BTBZMqHH6r4Tts7J_aSIgg";
const char* const kSalt = "DGv6ra1nlYgDCS1FRnbzlw";
const char* const kBody = "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";

std::string b64(const char* text) {
	std::string out;
	base64_decode(text, out);
	return out;
}

//受信側 (ブラウザ) の手順で復号する
std::string decrypt(const std::string& body, const std::string& ua_private, const std::string& auth_secret) {
	const std::string salt = body.substr(0, 16);
	const std::size_t idlen = static_cast<unsigned char>(body[20]);
	const std::string as_public = body.substr(21, idlen);
	const std::string ct = body.substr(21 + idlen);

	P256KeyPair ua;
	ua.private_key = ua_private;
	ua.public_key = p256_public_from_private(ua_private);
	const std::string shared = p256_ecdh(ua, as_public);
	const std::string ikm = hkdf_sha256(auth_secret, shared, std::string("WebPush: info\0", 14) + ua.public_key + as_public, 32);
	const std::string cek = hkdf_sha256(salt, ikm, std::string("Content-Encoding: aes128gcm\0", 28), 16);
	const std::string nonce = hkdf_sha256(salt, ikm, std::string("Content-Encoding: nonce\0", 24), 12);

	std::string plain(ct.size(), '\0');
	int len = 0;
	int tail = 0;
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, reinterpret_cast<const unsigned char*>(cek.data()), reinterpret_cast<const unsigned char*>(nonce.data()));
	EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&plain[0]), &len, reinterpret_cast<const unsigned char*>(ct.data()), static_cast<int>(ct.size() - 16));
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<char*>(ct.data() + ct.size() - 16));
	const int ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&plain[0]) + len, &tail);
	EVP_CIPHER_CTX_free(ctx);
	if (ok != 1) return "<auth failed>";
	plain.resize(static_cast<std::size_t>(len + tail));
	if (plain.empty() || plain.back() != 0x02) return "<bad padding>";
	plain.pop_back();
	return plain;
}

}

TEST(push_rfc8291, "Web Push: aes128gcm 暗号化が RFC 8291 の検証値と一致する") {
	EncryptOptions options;
	options.salt = b64(kSalt);
	options.server_private_key = b64(kAsPrivate);
	const std::string body = encrypt_payload({ kUaPublic, kAuthSecret }, kPlaintext, options);
	CHECK_EQ(base64url_encode(body), kBody);
}

TEST(push_random_keys, "Web Push: ランダムな鍵で暗号化しても受信側で復号できる") {
	const std::string message = "{\"title\":\"新しい通知\"}";
	const std::string body = encrypt_payload({ kUaPublic, kAuthSecret }, message);
	CHECK_EQ(decrypt(body, b64(kUaPrivate), b64(kAuthSecret)), message);
	CHECK(body != encrypt_payload({ kUaPublic, kAuthSecret }, message));	//毎回違う鍵と salt
}

TEST(push_reject, "Web Push: 大きすぎる本文と不正な鍵は拒否する") {
	CHECK_THROWS(encrypt_payload({ kUaPublic, kAuthSecret }, std::string(5000, 'x')));
	CHECK_THROWS(encrypt_payload({ "AAAA", kAuthSecret }, "x"));
	CHECK_THROWS(encrypt_payload({ kUaPublic, "AAAA" }, "x"));
	//曲線上に無い点
	CHECK_THROWS(encrypt_payload({ base64url_encode(std::string(1, '\x04') + std::string(64, '\x01')), kAuthSecret }, "x"));
}

TEST(push_vapid, "Web Push: VAPID の JWT は公開鍵で検証できる") {
	const P256KeyPair pair = p256_generate();
	VapidKeys keys;
	keys.subject = "mailto:info@test.com";
	keys.public_key = base64url_encode(pair.public_key);
	keys.private_key = base64url_encode(pair.private_key);

	const std::string header = create_vapid_authorization(keys, "https://fcm.googleapis.com", 3600, 1700000000000LL);
	CHECK(header.rfind("vapid t=", 0) == 0);
	const std::size_t comma = header.find(", k=");
	CHECK(comma != std::string::npos);
	CHECK_EQ(header.substr(comma + 4), keys.public_key);

	const std::string jwt = header.substr(8, comma - 8);
	const std::size_t dot1 = jwt.find('.');
	const std::size_t dot2 = jwt.find('.', dot1 + 1);
	const Json head = Json::parse(b64(jwt.substr(0, dot1).c_str()));
	const Json claims = Json::parse(b64(jwt.substr(dot1 + 1, dot2 - dot1 - 1).c_str()));
	CHECK(head == Json::object({ { "typ", "JWT" }, { "alg", "ES256" } }));
	CHECK(claims == Json::object({ { "aud", "https://fcm.googleapis.com" }, { "exp", 1700003600 }, { "sub", "mailto:info@test.com" } }));

	const std::string signature = b64(jwt.substr(dot2 + 1).c_str());
	CHECK_EQ(signature.size(), 64u);
	CHECK(p256_verify_sha256(pair.public_key, jwt.substr(0, dot2), signature));
	CHECK(!p256_verify_sha256(pair.public_key, jwt.substr(0, dot2) + "x", signature));

	keys.subject = "javascript:alert(1)";
	CHECK_THROWS(create_vapid_authorization(keys, "https://fcm.googleapis.com"));
}

TEST(push_endpoint, "Web Push: エンドポイントは https の既知サービスだけ許可する") {
	CHECK(is_allowed_endpoint("https://fcm.googleapis.com/fcm/send/abc"));
	CHECK(is_allowed_endpoint("https://wns2-par02p.notify.windows.com/w/?token=x"));
	CHECK(is_allowed_endpoint("https://FCM.googleapis.com/fcm/send/abc"));
	CHECK(!is_allowed_endpoint("http://fcm.googleapis.com/fcm/send/abc"));
	CHECK(!is_allowed_endpoint("https://169.254.169.254/latest/meta-data/"));
	CHECK(!is_allowed_endpoint("https://evil.example/fcm.googleapis.com"));
	CHECK(!is_allowed_endpoint("https://user:pw@fcm.googleapis.com/"));
	CHECK(!is_allowed_endpoint("https://fcm.googleapis.com.evil.example/"));
	CHECK(!is_allowed_endpoint("https://evilnotify.windows.com.example/"));
	CHECK(!is_allowed_endpoint("not a url"));

	PushSubscription sub;
	sub.endpoint = "https://evil.example/push";
	sub.keys = { kUaPublic, kAuthSecret };
	bool refused = false;
	try {
		send_notification(sub, "x", VapidKeys{});
	} catch (const PushError& ex) {
		refused = ex.status_code == 0;
	}
	CHECK(refused);	//通信する前に断る
}
