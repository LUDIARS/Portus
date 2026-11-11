#pragma once
//暗号まわりの入口。プリミティブは OpenSSL (libcrypto) を呼ぶだけで、自作しない
//バイト列は std::string で持つ
#include <cstddef>
#include <string>
#include <string_view>

namespace portus {

std::string random_bytes(std::size_t n);
std::string sha1(std::string_view data);
std::string sha256(std::string_view data);
std::string hmac_sha256(std::string_view key, std::string_view data);
std::string hkdf_sha256(std::string_view salt, std::string_view ikm, std::string_view info, std::size_t length);

//AES-128-GCM。戻り値は 暗号文 + 16 バイトのタグ
std::string aes128gcm_encrypt(std::string_view key, std::string_view nonce, std::string_view plaintext);

//P-256。秘密鍵は 32 バイト、公開鍵は 65 バイトの非圧縮形式 (0x04 || X || Y)
struct P256KeyPair {
	std::string private_key;
	std::string public_key;
};
P256KeyPair p256_generate();
std::string p256_public_from_private(std::string_view private_key);
std::string p256_ecdh(const P256KeyPair& own, std::string_view peer_public);
std::string p256_sign_sha256(const P256KeyPair& key, std::string_view message);	//64 バイト (r || s)
bool p256_verify_sha256(std::string_view public_key, std::string_view message, std::string_view signature);

std::string hex(std::string_view data);
std::string base64_encode(std::string_view data);
std::string base64url_encode(std::string_view data);			//パディング無し
bool base64_decode(std::string_view text, std::string& out);	//標準 / URL 安全のどちらも受ける。不正なら false

}
