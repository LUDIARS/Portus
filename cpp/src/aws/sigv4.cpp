#include "portus/aws.hpp"

#include <algorithm>
#include <map>

#include "portus/crypto.hpp"
#include "portus/util.hpp"

namespace portus {

std::string sha256_hex(std::string_view data) {
	return hex(sha256(data));
}

std::string encode_key_path(std::string_view key) {
	static const char* digits = "0123456789ABCDEF";
	std::string out;
	for (const char ch : key) {
		const unsigned char c = static_cast<unsigned char>(ch);
		const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
		if (keep) {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(digits[c >> 4]);
			out.push_back(digits[c & 0x0F]);
		}
	}
	return out;
}

//20130524T000000Z
static std::string amz_date(std::int64_t epoch_ms) {
	const std::string iso = iso_time(epoch_ms);	//2013-05-24T00:00:00.000Z
	return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2) + "T" + iso.substr(11, 2) + iso.substr(14, 2) + iso.substr(17, 2) + "Z";
}

//前後の空白を落とし、連続する空白を 1 つにする
static std::string canonical_value(std::string_view value) {
	std::string out;
	bool space = false;
	for (const char c : trim(value)) {
		const bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
		if (ws) {
			space = true;
			continue;
		}
		if (space) out.push_back(' ');
		space = false;
		out.push_back(c);
	}
	return out;
}

HeaderList sign_request(const SignInput& input, const AwsCredentials& creds) {
	const std::string date = amz_date(input.date_ms < 0 ? now_ms() : input.date_ms);
	const std::string day = date.substr(0, 8);
	const std::string payload_hash = sha256_hex(input.payload);

	std::map<std::string, std::string> headers;	//小文字名で昇順
	for (const auto& kv : input.headers) headers[to_lower(kv.first)] = canonical_value(kv.second);
	headers["host"] = input.host;
	headers["x-amz-content-sha256"] = payload_hash;
	headers["x-amz-date"] = date;
	if (!creds.session_token.empty()) headers["x-amz-security-token"] = creds.session_token;

	std::string canonical_headers;
	std::string signed_headers;
	for (const auto& kv : headers) {
		canonical_headers += kv.first + ":" + kv.second + "\n";
		if (!signed_headers.empty()) signed_headers += ";";
		signed_headers += kv.first;
	}
	const std::string canonical_request = input.method + "\n" + input.path + "\n\n" + canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

	const std::string scope = day + "/" + input.region + "/" + input.service + "/aws4_request";
	const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + date + "\n" + scope + "\n" + sha256_hex(canonical_request);

	const std::string k_date = hmac_sha256("AWS4" + creds.secret_access_key, day);
	const std::string k_region = hmac_sha256(k_date, input.region);
	const std::string k_service = hmac_sha256(k_region, input.service);
	const std::string k_signing = hmac_sha256(k_service, "aws4_request");
	const std::string signature = hex(hmac_sha256(k_signing, string_to_sign));

	HeaderList out;
	for (const auto& kv : headers) {
		if (kv.first != "host") out.emplace_back(kv.first, kv.second);
	}
	out.emplace_back("authorization", "AWS4-HMAC-SHA256 Credential=" + creds.access_key_id + "/" + scope + ", SignedHeaders=" + signed_headers + ", Signature=" + signature);
	return out;
}

}
