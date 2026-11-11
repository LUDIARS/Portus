#include "portus/crypto.hpp"

#include <memory>
#include <stdexcept>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

namespace portus {

namespace {

[[noreturn]] void fail(const char* what) {
	throw std::runtime_error(std::string("crypto: ") + what);
}

const unsigned char* bytes(std::string_view s) {
	return reinterpret_cast<const unsigned char*>(s.data());
}

template <class T, void (*Free)(T*)>
struct Deleter {
	void operator()(T* p) const { Free(p); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, Deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, Deleter<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;
using BnPtr = std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_clear_free>>;
using ParamBldPtr = std::unique_ptr<OSSL_PARAM_BLD, Deleter<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>>;
using ParamPtr = std::unique_ptr<OSSL_PARAM, Deleter<OSSL_PARAM, OSSL_PARAM_free>>;

std::string digest(const EVP_MD* md, std::string_view data) {
	unsigned char out[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (EVP_Digest(data.data(), data.size(), out, &len, md, nullptr) != 1) fail("digest failed");
	return std::string(reinterpret_cast<char*>(out), len);
}

//公開鍵 (必須) と秘密鍵 (任意) から EVP_PKEY を作る
PkeyPtr make_p256(std::string_view public_key, std::string_view private_key) {
	if (public_key.size() != 65 || public_key[0] != 0x04) fail("P-256 public key must be 65-byte uncompressed");
	if (!private_key.empty() && private_key.size() != 32) fail("P-256 private key must be 32 bytes");

	ParamBldPtr bld(OSSL_PARAM_BLD_new());
	BnPtr d;
	if (!bld) fail("alloc failed");
	OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
	OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, public_key.data(), public_key.size());
	if (!private_key.empty()) {
		d.reset(BN_bin2bn(bytes(private_key), static_cast<int>(private_key.size()), nullptr));
		if (!d) fail("alloc failed");
		OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, d.get());
	}
	ParamPtr params(OSSL_PARAM_BLD_to_param(bld.get()));
	PkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
	if (!params || !ctx) fail("alloc failed");

	EVP_PKEY* raw = nullptr;
	if (EVP_PKEY_fromdata_init(ctx.get()) != 1
		|| EVP_PKEY_fromdata(ctx.get(), &raw, private_key.empty() ? EVP_PKEY_PUBLIC_KEY : EVP_PKEY_KEYPAIR, params.get()) != 1) {
		fail("invalid P-256 key");
	}
	return PkeyPtr(raw);
}

}

std::string random_bytes(std::size_t n) {
	std::string out(n, '\0');
	if (n > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(&out[0]), static_cast<int>(n)) != 1) fail("random failed");
	return out;
}

std::string sha1(std::string_view data) {
	return digest(EVP_sha1(), data);
}

std::string sha256(std::string_view data) {
	return digest(EVP_sha256(), data);
}

std::string hmac_sha256(std::string_view key, std::string_view data) {
	unsigned char out[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), bytes(data), data.size(), out, &len)) fail("hmac failed");
	return std::string(reinterpret_cast<char*>(out), len);
}

std::string hkdf_sha256(std::string_view salt, std::string_view ikm, std::string_view info, std::size_t length) {
	PkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
	std::string out(length, '\0');
	std::size_t len = length;
	if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1
		|| EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) != 1
		|| EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), bytes(salt), static_cast<int>(salt.size())) != 1
		|| EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), bytes(ikm), static_cast<int>(ikm.size())) != 1
		|| EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), bytes(info), static_cast<int>(info.size())) != 1
		|| EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]), &len) != 1) {
		fail("hkdf failed");
	}
	return out;
}

std::string aes128gcm_encrypt(std::string_view key, std::string_view nonce, std::string_view plaintext) {
	if (key.size() != 16 || nonce.size() != 12) fail("aes128gcm needs 16-byte key and 12-byte nonce");
	CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
	std::string out(plaintext.size() + 16, '\0');
	unsigned char* dst = reinterpret_cast<unsigned char*>(&out[0]);
	int len = 0;
	int tail = 0;
	if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, bytes(key), bytes(nonce)) != 1
		|| EVP_EncryptUpdate(ctx.get(), dst, &len, bytes(plaintext), static_cast<int>(plaintext.size())) != 1
		|| EVP_EncryptFinal_ex(ctx.get(), dst + len, &tail) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, dst + plaintext.size()) != 1) {
		fail("aes128gcm failed");
	}
	return out;
}

std::string p256_public_from_private(std::string_view private_key) {
	if (private_key.size() != 32) fail("P-256 private key must be 32 bytes");
	BnPtr d(BN_bin2bn(bytes(private_key), 32, nullptr));
	EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	EC_POINT* point = group ? EC_POINT_new(group) : nullptr;
	std::string out(65, '\0');
	const bool ok = d && point
		&& EC_POINT_mul(group, point, d.get(), nullptr, nullptr, nullptr) == 1
		&& EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, reinterpret_cast<unsigned char*>(&out[0]), 65, nullptr) == 65;
	EC_POINT_free(point);
	EC_GROUP_free(group);
	if (!ok) fail("invalid P-256 private key");
	return out;
}

P256KeyPair p256_generate() {
	PkeyPtr key(EVP_EC_gen("P-256"));
	if (!key) fail("key generation failed");

	BIGNUM* raw = nullptr;
	if (EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_PRIV_KEY, &raw) != 1) fail("key export failed");
	BnPtr d(raw);

	P256KeyPair pair;
	pair.private_key.assign(32, '\0');
	if (BN_bn2binpad(d.get(), reinterpret_cast<unsigned char*>(&pair.private_key[0]), 32) != 32) fail("key export failed");
	pair.public_key = p256_public_from_private(pair.private_key);
	return pair;
}

std::string p256_ecdh(const P256KeyPair& own, std::string_view peer_public) {
	PkeyPtr key = make_p256(own.public_key, own.private_key);
	PkeyPtr peer = make_p256(peer_public, std::string_view());
	PkeyCtxPtr ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
	std::string out(32, '\0');
	std::size_t len = out.size();
	if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1
		|| EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1
		|| EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]), &len) != 1 || len != 32) {
		fail("ecdh failed");
	}
	return out;
}

std::string p256_sign_sha256(const P256KeyPair& key, std::string_view message) {
	PkeyPtr pkey = make_p256(key.public_key, key.private_key);
	MdCtxPtr ctx(EVP_MD_CTX_new());
	unsigned char der[128];
	std::size_t der_len = sizeof(der);
	if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1
		|| EVP_DigestSign(ctx.get(), der, &der_len, bytes(message), message.size()) != 1) {
		fail("sign failed");
	}

	//DER (r, s) を JWS が使う固定長の r || s にする
	const unsigned char* p = der;
	ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der_len));
	if (!sig) fail("sign failed");
	std::string out(64, '\0');
	unsigned char* dst = reinterpret_cast<unsigned char*>(&out[0]);
	const bool ok = BN_bn2binpad(ECDSA_SIG_get0_r(sig), dst, 32) == 32 && BN_bn2binpad(ECDSA_SIG_get0_s(sig), dst + 32, 32) == 32;
	ECDSA_SIG_free(sig);
	if (!ok) fail("sign failed");
	return out;
}

bool p256_verify_sha256(std::string_view public_key, std::string_view message, std::string_view signature) {
	if (signature.size() != 64) return false;
	PkeyPtr pkey = make_p256(public_key, std::string_view());

	ECDSA_SIG* sig = ECDSA_SIG_new();
	BIGNUM* r = BN_bin2bn(bytes(signature), 32, nullptr);
	BIGNUM* s = BN_bin2bn(bytes(signature) + 32, 32, nullptr);
	if (!sig || !r || !s || ECDSA_SIG_set0(sig, r, s) != 1) {
		ECDSA_SIG_free(sig);
		fail("alloc failed");
	}
	unsigned char* der = nullptr;
	const int der_len = i2d_ECDSA_SIG(sig, &der);
	ECDSA_SIG_free(sig);
	if (der_len <= 0) return false;

	MdCtxPtr ctx(EVP_MD_CTX_new());
	const bool ok = ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) == 1
		&& EVP_DigestVerify(ctx.get(), der, static_cast<std::size_t>(der_len), bytes(message), message.size()) == 1;
	OPENSSL_free(der);
	return ok;
}

}
