#pragma once
//各 AI / Notion API の薄いクライアント。使っているエンドポイントだけ。呼び出しは完了まで戻らない
#include <string>

#include "portus/json.hpp"

namespace portus {

class OpenAIClient {
public:
	explicit OpenAIClient(std::string api_key) : api_key_(std::move(api_key)) {}
	Json list_models() const;							//GET /models → { data: [...] }
	Json chat_completions(const Json& body) const;		//POST /chat/completions
	Json create_realtime_session(const Json& body) const;	//POST /realtime/sessions (エフェメラルキー発行)

private:
	std::string api_key_;
};

class AnthropicClient {
public:
	explicit AnthropicClient(std::string api_key) : api_key_(std::move(api_key)) {}
	Json list_models() const;						//GET /models → { data: [...] }
	Json create_message(const Json& body) const;	//POST /messages

private:
	std::string api_key_;
};

//キーは URL ではなくヘッダで渡す (ログに残さないため)
class GeminiClient {
public:
	explicit GeminiClient(std::string api_key) : api_key_(std::move(api_key)) {}
	Json list_models() const;	//GET /models → { models: [...] }
	//POST /models/{model}:generateContent の応答テキスト
	std::string generate_text(const std::string& model, const Json& body) const;

private:
	std::string api_key_;
};

class NotionClient {
public:
	explicit NotionClient(std::string token) : token_(std::move(token)) {}
	Json users_list(const std::string& start_cursor = "") const;
	Json databases_query(const std::string& database_id, const Json& body = Json::object()) const;
	Json blocks_children_list(const std::string& block_id, const std::string& start_cursor = "") const;
	Json pages_retrieve(const std::string& page_id) const;
	Json pages_create(const Json& data) const;
	Json search(const Json& params) const;

private:
	Json call(const std::string& path, const Json* body) const;
	std::string token_;
};

}
