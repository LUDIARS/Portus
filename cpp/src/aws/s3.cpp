#include "portus/aws.hpp"

#include <stdexcept>

#include "portus/net.hpp"

namespace portus {

//[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]
static bool valid_bucket(const std::string& bucket) {
	if (bucket.size() < 3 || bucket.size() > 63) return false;
	const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
	if (!alnum(bucket.front()) || !alnum(bucket.back())) return false;
	for (const char c : bucket) {
		if (!alnum(c) && c != '.' && c != '-') return false;
	}
	return true;
}

//ホスト名に埋め込むので、リージョンも英小文字・数字・ハイフンに限る
static bool valid_region(const std::string& region) {
	if (region.empty() || region.size() > 32) return false;
	for (const char c : region) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
	}
	return true;
}

void put_object(const std::string& region, const std::string& bucket, const std::string& key, const std::string& body, const std::string& content_type) {
	if (!valid_bucket(bucket)) throw std::invalid_argument("invalid bucket name " + bucket);
	if (!valid_region(region)) throw std::invalid_argument("invalid region " + region);

	const std::string host = bucket + ".s3." + region + ".amazonaws.com";
	const std::string path = "/" + encode_key_path(key);

	SignInput input;
	input.method = "PUT";
	input.host = host;
	input.path = path;
	input.region = region;
	input.service = "s3";
	input.payload = body;
	input.headers = { { "content-type", content_type } };

	FetchRequest req;
	req.method = "PUT";
	req.timeout_ms = 30000;
	req.headers = sign_request(input, resolve_credentials());
	req.body = body;

	const FetchResponse res = fetch("https://" + host + path, req);
	if (!res.ok()) throw std::runtime_error("S3 PutObject failed " + std::to_string(res.status) + ": " + res.body.substr(0, 300));
}

}
