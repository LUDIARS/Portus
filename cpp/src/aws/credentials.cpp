#include "portus/aws.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "expiration.hpp"
#include "portus/env.hpp"
#include "portus/json.hpp"
#include "portus/net.hpp"
#include "portus/util.hpp"
#include "augur/contract_runtime.hpp" /* augur-inject:import:0328e828 */
#include "expiration.contract.hpp" /* augur-inject:contract-predicate:183f008d */

namespace portus {

namespace detail {

static int read_decimal(std::string_view field) {
	unsigned value = 0;
	const char* const end = field.data() + field.size();
	const auto parsed = std::from_chars(field.data(), end, value);
	if (parsed.ec != std::errc{} || parsed.ptr != end) return -1;
	return static_cast<int>(value);
}

// Read the UTC timestamp through whole seconds, as in the previous IMDS parser.
std::int64_t parse_iso_time(std::string_view text) { return ::augur::invoke_observed(::augur::ContractSpec{"C-5", "c42bc626", "cpp/src/aws/credentials.cpp:22"}, contracts::expiration_checks, [&]() -> std::int64_t { /* augur-inject:contract-wrap:c42bc626 */
	using namespace std::chrono;
	constexpr std::string_view pattern = "yyyy-mm-ddThh:mm:ss";
	if (text.size() < pattern.size()) return 0;
	for (const std::size_t offset : std::array<std::size_t, 5>{4, 7, 10, 13, 16}) {
		if (text[offset] != pattern[offset]) return 0;
	}

	const int years = read_decimal(text.substr(0, 4));
	const int months = read_decimal(text.substr(5, 2));
	const int days = read_decimal(text.substr(8, 2));
	const int hour = read_decimal(text.substr(11, 2));
	const int minute = read_decimal(text.substr(14, 2));
	const int second = read_decimal(text.substr(17, 2));
	if (years < 0 || months < 1 || days < 1 || hour < 0 || hour > 23
		|| minute < 0 || minute > 59 || second < 0 || second > 60) return 0;

	const year_month_day date{year{years}, month{static_cast<unsigned>(months)}, day{static_cast<unsigned>(days)}};
	if (!date.ok()) return 0;
	const auto instant = sys_days{date} + hours{hour} + minutes{minute} + seconds{second};
	return duration_cast<milliseconds>(instant.time_since_epoch()).count();
}, text); /* augur-inject:contract-end:c42bc626 */ }

}

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
		out.expiration_ms = detail::parse_iso_time(c.at("Expiration").as_string());
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
