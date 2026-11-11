#pragma once
//AWS Signature Version 4 (ヘッダ署名)、認証情報の解決、S3 PutObject
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "portus/http_message.hpp"

namespace portus {

struct AwsCredentials {
	std::string access_key_id;
	std::string secret_access_key;
	std::string session_token;			//無ければ空
	std::int64_t expiration_ms = 0;		//0 は期限なし
};

struct SignInput {
	std::string method;
	std::string host;
	std::string path;		//エンコード済みのパス
	std::string region;
	std::string service;
	std::string payload;
	HeaderList headers;
	std::int64_t date_ms = -1;	//負なら現在時刻
};

std::string sha256_hex(std::string_view data);

//RFC 3986 の非予約文字以外をエンコードする (S3 のキー用。"/" は区切りとして残す)
std::string encode_key_path(std::string_view key);

//署名済みヘッダ (Host を除く。名前は小文字で昇順) を返す
HeaderList sign_request(const SignInput& input, const AwsCredentials& creds);

//ini の該当プロファイルだけを読む
std::optional<AwsCredentials> parse_credentials_file(std::string_view text, const std::string& profile);

//環境変数 → 共有認証ファイル (~/.aws/credentials) → EC2 インスタンスメタデータ (IMDSv2) の順。見つからなければ例外
AwsCredentials resolve_credentials();

//S3 PutObject。失敗は例外
void put_object(const std::string& region, const std::string& bucket, const std::string& key, const std::string& body, const std::string& content_type = "text/plain; charset=utf-8");

}
