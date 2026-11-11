#pragma once
//HTTP サーバの契約 (ルート定義・要求の文脈・応答) と表駆動ルータ
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "portus/event_loop.hpp"
#include "portus/http_message.hpp"
#include "portus/json.hpp"
#include "portus/log.hpp"

namespace portus {

//ステータス・本文・ヘッダを明示した応答。文字列からは text/plain、Json からは JSON の 200 になる
struct Reply {
	int status = 200;
	std::string body;
	HeaderList headers;

	Reply() = default;
	Reply(int status_, std::string body_ = "", HeaderList headers_ = {});
	Reply(const char* text_body);
	Reply(std::string text_body);
	Reply(const Json& data);

	static Reply no_content() { return Reply(204); }
	static Reply bytes(std::string data);	//application/octet-stream
};

Reply json(const Json& data, int status = 200, HeaderList headers = {});
Reply text(std::string body, int status = 200, HeaderList headers = {});
Reply html(std::string body, int status = 200, HeaderList headers = {});

struct RequestContext;
using Handler = std::function<Reply(RequestContext&)>;
//next() でハンドラ (または次のミドルウェア) を呼ぶ。呼ばずに Reply を返せば打ち切り
using Next = std::function<Reply()>;
using Middleware = std::function<Reply(RequestContext&, const Next&)>;

//"モジュール#関数" → 実体。表に載せた名前だけが呼べる
using HandlerTable = std::map<std::string, Handler>;

//ルート定義。表に書いたものだけが到達可能で、上から順に最初に一致したものを使う
//  path:    {name} は文字列変数、{name:number} は数値変数 (整数・小数)
//  handler: 関数、または "モジュール#関数" (HandlerTable から引く)
struct RouteDef {
	std::string method;
	std::string path;
	Handler handler;
	std::string handler_ref;
	Json meta = Json::object();	//ミドルウェアが参照する任意の印 (認証要否など)
	std::string note;

	RouteDef(std::string method_, std::string path_, Handler handler_, Json meta_ = Json::object(), std::string note_ = "");
	RouteDef(std::string method_, std::string path_, const char* handler_ref_, Json meta_ = Json::object(), std::string note_ = "");
	RouteDef(std::string method_, std::string path_, std::string handler_ref_, Json meta_ = Json::object(), std::string note_ = "");

	std::string handler_name() const { return handler_ref.empty() ? "fn" : handler_ref; }
};

//1 リクエストの文脈。ハンドラは既定ではワーカースレッドで呼ばれる (ブロックしてよい)
struct RequestContext {
	std::string req_id;
	std::string method;
	std::string path;
	std::string remote_addr;
	Headers headers;
	std::shared_ptr<const RouteDef> route;
	Json params = Json::object();	//パス変数だけ
	Json query = Json::object();	//クエリ + ボディ (JSON オブジェクト / フォーム) + パス変数。同じキーはパス変数が勝つ
	std::string body;				//生のボディ (POST / PUT / PATCH / DELETE 以外は空)
	Json state = Json::object();	//ミドルウェア間の受け渡し
	Logger log;
};

struct RouteMatch {
	std::shared_ptr<const RouteDef> def;
	Json params;
	Json query;
};

//クエリ文字列をオブジェクトにする (__proto__ などのキーは捨てる)
Json parse_query_string(std::string_view qs);

//ボディを dst へ足す。JSON オブジェクトを優先し、JSON でなければ x-www-form-urlencoded として読む
//  skip に載っているキー (パス変数) は上書きしない
void merge_body(Json& dst, std::string_view text, const Json& skip = Json::object());

//表駆動のルータ。表に書いたものだけが一致し、親ルートへのフォールバックはしない
class Router {
public:
	explicit Router(std::vector<RouteDef> defs);	//不正なメソッド・ハンドラ名は std::invalid_argument

	//URL (パス + クエリ) を解決する
	std::optional<RouteMatch> match(std::string_view method, std::string_view url) const;

	//ルート表をそのまま返す (一覧 API などで公開できるように)
	std::vector<RouteDef> list() const;

private:
	struct Segment {
		bool is_param = false;
		bool is_number = false;
		std::string text;	//リテラル、または変数名
	};
	struct Compiled {
		std::shared_ptr<const RouteDef> def;
		std::vector<Segment> segments;
	};
	std::vector<Compiled> routes_;
};

struct HttpServerOptions {
	std::vector<RouteDef> routes;
	HandlerTable handlers;
	std::vector<Middleware> middlewares;
	Logger log;

	std::size_t max_body_bytes = 1024 * 1024;	//超えたら 413
	std::size_t max_header_bytes = 16 * 1024;	//超えたら 431
	std::size_t max_connections = 1024;			//超えた接続は受けてすぐ閉じる
	std::size_t worker_threads = 4;				//ハンドラを動かすスレッド数。0 ならループスレッドで直接動かす (ブロックしないハンドラだけの構成向け)
	std::size_t max_pending = 1024;				//ワーカー待ちの上限。溢れたら 503
	int request_timeout_ms = 30000;				//要求を受け切るまでの上限 (遅延送信で接続を占有されない)
	int keep_alive_timeout_ms = 5000;			//応答後、次の要求を待つ時間

	//空でなければ Host ヘッダ (ポートを除く) がこの中に無い要求を 421 で拒否する
	//ローカルサービスを直接ブラウザから叩ける場所で動かすときの DNS リバインディング対策
	std::vector<std::string> allowed_hosts;
};

//HTTP/1.1 サーバ。入出力はイベントループ、ハンドラはワーカースレッドで動く
//EventLoop より先に close (または破棄) すること
class HttpServer {
public:
	HttpServer(EventLoop& loop, HttpServerOptions options);
	~HttpServer();
	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

	int listen(int port, const std::string& host = "127.0.0.1");	//待ち受けたポートを返す (port = 0 で空きポート)
	void set_routes(std::vector<RouteDef> routes);	//次の要求から新しい表を使う。不正な表は例外で、前の表のまま
	std::vector<RouteDef> routes() const;
	void close();

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

}
