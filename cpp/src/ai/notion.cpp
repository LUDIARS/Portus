#include "portus/ai.hpp"

#include <stdexcept>

#include "portus/net.hpp"
#include "portus/util.hpp"

namespace portus {

static const char* const kBase = "https://api.notion.com/v1";
static const char* const kVersion = "2022-06-28";

//[0-9a-fA-F-]{32,36}
static const std::string& checked_id(const std::string& value) {
	bool ok = value.size() >= 32 && value.size() <= 36;
	for (const char c : value) {
		ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-');
	}
	if (!ok) throw std::invalid_argument("invalid notion id");
	return value;
}

static std::string cursor_query(const std::string& cursor) {
	return cursor.empty() ? "" : "?start_cursor=" + percent_encode_component(cursor);
}

Json NotionClient::call(const std::string& path, const Json* body) const {
	JsonRequest req;
	req.method = body ? "POST" : "GET";
	req.headers = { { "Authorization", "Bearer " + token_ }, { "Notion-Version", kVersion } };
	if (body) req.body = *body;
	return fetch_json(std::string(kBase) + path, req);
}

Json NotionClient::users_list(const std::string& start_cursor) const {
	return call("/users" + cursor_query(start_cursor), nullptr);
}

Json NotionClient::databases_query(const std::string& database_id, const Json& body) const {
	return call("/databases/" + checked_id(database_id) + "/query", &body);
}

Json NotionClient::blocks_children_list(const std::string& block_id, const std::string& start_cursor) const {
	return call("/blocks/" + checked_id(block_id) + "/children" + cursor_query(start_cursor), nullptr);
}

Json NotionClient::pages_retrieve(const std::string& page_id) const {
	return call("/pages/" + checked_id(page_id), nullptr);
}

Json NotionClient::pages_create(const Json& data) const {
	return call("/pages", &data);
}

Json NotionClient::search(const Json& params) const {
	return call("/search", &params);
}

}
