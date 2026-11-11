#pragma once
//Web Push 送信 (RFC 8291 の暗号化 + RFC 8292 VAPID)
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace portus {

struct SubscriptionKeys {
	std::string p256dh;	//base64url, ブラウザの公開鍵 (65 byte)
	std::string auth;	//base64url, 認証シークレット (16 byte)
};

//テストで固定値を差し込むためのもの (通常は空のまま)
struct EncryptOptions {
	std::string salt;				//16 byte
	std::string server_private_key;	//32 byte
};

//aes128gcm (単一レコード) で暗号化した本文を返す。鍵が不正・本文が大きすぎるときは例外
std::string encrypt_payload(const SubscriptionKeys& keys, std::string_view payload, const EncryptOptions& options = {});

struct VapidKeys {
	std::string subject;		//mailto: か https: の連絡先
	std::string public_key;		//base64url の非圧縮公開鍵 (65 byte)
	std::string private_key;	//base64url の秘密鍵 (32 byte)
};

//Authorization ヘッダの値。audience は push サービスの origin (例: https://fcm.googleapis.com)
std::string create_vapid_authorization(const VapidKeys& keys, const std::string& audience, long long ttl_sec = 12 * 60 * 60, long long now_ms = -1);

//購読エンドポイントはクライアントが送ってくる URL なので、既知の push サービス以外へは送らない (SSRF 対策)
//先頭 "." はサブドメイン一致
const std::vector<std::string>& default_push_hosts();
bool is_allowed_endpoint(const std::string& endpoint, const std::vector<std::string>& hosts = default_push_hosts());

struct PushSubscription {
	std::string endpoint;
	SubscriptionKeys keys;
};

struct SendOptions {
	long long ttl_sec = 24 * 60 * 60;		//push サービスでの保持秒数
	std::vector<std::string> allowed_hosts;	//空なら default_push_hosts()
	int timeout_ms = 10000;
};

//暗号化した本文を購読エンドポイントへ POST する。戻り値は HTTP ステータス
//許可されない送信先・2xx 以外は PushError
class PushError : public std::runtime_error {
public:
	PushError(const std::string& message, int status_code_) : std::runtime_error(message), status_code(status_code_) {}
	int status_code;
};
int send_notification(const PushSubscription& sub, std::string_view payload, const VapidKeys& vapid, const SendOptions& options = {});

}
