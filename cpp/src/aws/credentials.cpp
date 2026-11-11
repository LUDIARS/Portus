#include "portus/aws.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "portus/env.hpp"
#include "portus/json.hpp"
#include "portus/net.hpp"
#include "portus/util.hpp"

namespace portus {

namespace {

const char* const kImds = "http://169.254.169.254";

std::optional<AwsCredentials> from_env() {
	AwsCredentials c;
	c.access_key_id = get_env("AWS_ACCESS_KEY_ID");
	c.secret_access_key = get_env("AWS_SECRET_ACCESS_KEY");
	if (c.access_key_id.empty() || c.secret_access_key.empty()) return std::nullopt;
	c.session_token = get_env("AWS_SESSION_TOKEN");
	return c;
}

std::optional<AwsCredentials> from_shared_file() {
	std::string file = get_env("AWS_SHARED_CREDENTIALS_FILE");
	if (file.empty()) {
		const std::string home = get_env("HOME", get_env("USERPROFILE"));
		if (home.empty()) return std::nullopt;
		file = home + "/.aws/credentials";
	}
	std::ifstream in(file, std::ios::binary);
	if (!in) return std::nullopt;
	std::stringstream ss;
	ss << in.rdbuf();
	return parse_credentials_file(ss.str(), get_env("AWS_PROFILE", "default"));
}

//2026-01-02T03:04:05Z → エポックミリ秒 (読めなければ 0)
std::int64_t parse_iso_time(const std::string& text) {
	int y, mo, d, h, mi, s;
	if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
	//グレゴリオ暦の通日 (タイムゾーンに依存する mktime を避ける)
	const int a = (14 - mo) / 12;
	const long long yy = y + 4800 - a;
	const long long mm = mo + 12 * a - 3;
	const long long jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
	const long long days = jdn - 2440588;
	return (days * 86400 + h * 3600 + mi * 60 + s) * 1000;
}

std::optional<AwsCredentials> from_instance_metadata() {
	try {
		FetchRequest token_req;
		token_req.method = "PUT";
		token_req.timeout_ms = 1000;
		token_req.max_response_bytes = 64 * 1024;
		token_req.headers = { { "X-aws-ec2-metadata-token-ttl-seconds", "21600" } };
		const FetchResponse token_res = fetch(std::string(kImds) + "/latest/api/token", token_req);
		if (!token_res.ok()) return std::nullopt;

		FetchRequest req;
		req.timeout_ms = 1000;
		req.max_response_bytes = 64 * 1024;
		req.headers = { { "X-aws-ec2-metadata-token", std::string(trim(token_res.body)) } };
		const FetchResponse role_res = fetch(std::string(kImds) + "/latest/meta-data/iam/security-credentials/", req);
		if (!role_res.ok()) return std::nullopt;
		const std::string role(trim(split(role_res.body, '\n').front()));
		if (role.empty()) return std::nullopt;

		const FetchResponse cred_res = fetch(std::string(kImds) + "/latest/meta-data/iam/security-credentials/" + percent_encode_component(role), req);
		if (!cred_res.ok()) return std::nullopt;
		const Json c = Json::parse(cred_res.body);

		AwsCredentials out;
		out.access_key_id = c.at("AccessKeyId").as_string();
		out.secret_access_key = c.at("SecretAccessKey").as_string();
		out.session_token = c.at("Token").as_string();
		out.expiration_ms = parse_iso_time(c.at("Expiration").as_string());
		if (out.access_key_id.empty() || out.secret_access_key.empty()) return std::nullopt;
		return out;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

bool is_fresh(const AwsCredentials& c) {
	return c.expiration_ms == 0 || c.expiration_ms - now_ms() > 5 * 60 * 1000;
}

}

std::optional<AwsCredentials> parse_credentials_file(std::string_view text, const std::string& profile) {
	std::string current;
	std::map<std::string, std::string> values;
	for (const std::string& raw : split(text, '\n')) {
		const std::string_view line = trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
			current = std::string(trim(line.substr(1, line.size() - 2)));
			continue;
		}
		if (current != profile) continue;
		const std::size_t eq = line.find('=');
		if (eq != std::string_view::npos && eq > 0) values[std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
	}
	if (values["aws_access_key_id"].empty() || values["aws_secret_access_key"].empty()) return std::nullopt;

	AwsCredentials c;
	c.access_key_id = values["aws_access_key_id"];
	c.secret_access_key = values["aws_secret_access_key"];
	c.session_token = values["aws_session_token"];
	return c;
}

AwsCredentials resolve_credentials() {
	static std::mutex mu;
	static std::optional<AwsCredentials> cached;

	std::lock_guard<std::mutex> lock(mu);
	if (cached && is_fresh(*cached)) return *cached;

	cached = from_env();
	if (!cached) cached = from_shared_file();
	if (!cached) cached = from_instance_metadata();
	if (!cached) throw std::runtime_error("AWS credentials not found");
	return *cached;
}

}
