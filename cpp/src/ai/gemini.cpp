#include "portus/ai.hpp"

#include "portus/net.hpp"
#include "portus/util.hpp"

namespace portus {

static const char* const kBase = "https://generativelanguage.googleapis.com/v1beta";

Json GeminiClient::list_models() const {
	JsonRequest req;
	req.headers = { { "x-goog-api-key", api_key_ } };
	return fetch_json(std::string(kBase) + "/models?pageSize=1000", req);
}

std::string GeminiClient::generate_text(const std::string& model, const Json& body) const {
	const std::string name = path_segment(starts_with(model, "models/") ? model.substr(7) : model, "model");

	JsonRequest req;
	req.headers = { { "x-goog-api-key", api_key_ } };
	req.body = body;
	const Json res = fetch_json(std::string(kBase) + "/models/" + name + ":generateContent", req);

	std::string out;
	const Json::Array& candidates = res.at("candidates").items();
	if (candidates.empty()) return out;
	for (const Json& part : candidates[0].at("content").at("parts").items()) out += part.at("text").as_string();
	return out;
}

}
