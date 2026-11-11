#include "portus/ai.hpp"

#include <stdexcept>

#include "portus/net.hpp"

namespace portus {

static const char* const kBase = "https://api.openai.com/v1";

static JsonRequest request(const std::string& api_key, const Json* body) {
	JsonRequest req;
	req.headers = { { "Authorization", "Bearer " + api_key } };
	if (body) req.body = *body;
	return req;
}

Json OpenAIClient::list_models() const {
	return fetch_json(std::string(kBase) + "/models", request(api_key_, nullptr));
}

Json OpenAIClient::chat_completions(const Json& body) const {
	const Json& model = body.at("model");
	if (!model.is_string() || model.as_string().empty() || model.as_string().size() > 128) throw std::invalid_argument("invalid model");
	return fetch_json(std::string(kBase) + "/chat/completions", request(api_key_, &body));
}

Json OpenAIClient::create_realtime_session(const Json& body) const {
	return fetch_json(std::string(kBase) + "/realtime/sessions", request(api_key_, &body));
}

}
