#include "portus/ai.hpp"

#include <stdexcept>

#include "portus/net.hpp"

namespace portus {

static const char* const kBase = "https://api.anthropic.com/v1";
static const char* const kVersion = "2023-06-01";

static JsonRequest request(const std::string& api_key, const Json* body) {
	JsonRequest req;
	req.headers = { { "x-api-key", api_key }, { "anthropic-version", kVersion } };
	if (body) req.body = *body;
	return req;
}

Json AnthropicClient::list_models() const {
	return fetch_json(std::string(kBase) + "/models?limit=1000", request(api_key_, nullptr));
}

Json AnthropicClient::create_message(const Json& body) const {
	const Json& model = body.at("model");
	if (!model.is_string() || model.as_string().empty() || model.as_string().size() > 128) throw std::invalid_argument("invalid model");
	return fetch_json(std::string(kBase) + "/messages", request(api_key_, &body));
}

}
