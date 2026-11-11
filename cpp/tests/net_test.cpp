#include <cstdio>
#include <cstring>
#include <filesystem>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "portus/http.hpp"
#include "portus/id.hpp"
#include "portus/net.hpp"
#include "portus/socket.hpp"
#include "test.hpp"

using namespace portus;

namespace {

//要求を読み捨てて、決まったバイト列を返すだけのサーバ
struct CannedServer {
	Listener listener;
	std::thread thread;
	std::atomic<bool> stop{false};
	std::string response;
	bool close_after = true;

	explicit CannedServer(std::string response_, bool close_after_ = true)
		: listener(listen_tcp("127.0.0.1", 0)), response(std::move(response_)), close_after(close_after_) {
		thread = std::thread([this] { serve(); });
	}
	~CannedServer() {
		stop = true;
		thread.join();
		close_socket(listener.fd);
	}
	std::string url() const { return "http://127.0.0.1:" + std::to_string(listener.port) + "/x"; }

	void serve() {
		std::vector<socket_t> held;
		while (!stop) {
			if (!wait_socket(listener.fd, kRead, 20)) continue;
			const socket_t fd = accept_tcp(listener.fd, nullptr);
			if (fd == kInvalidSocket) continue;
			std::string in;
			char buf[4096];
			while (in.find("\r\n\r\n") == std::string::npos && wait_socket(fd, kRead, 1000)) {
				const long n = socket_recv(fd, buf, sizeof(buf));
				if (n <= 0) break;
				in.append(buf, static_cast<std::size_t>(n));
			}
			socket_send(fd, response.data(), response.size());
			if (close_after) close_socket(fd);
			else held.push_back(fd);
		}
		for (const socket_t fd : held) close_socket(fd);
	}
};

std::string error_of(const std::string& url, const FetchRequest& req = {}) {
	try {
		fetch(url, req);
	} catch (const FetchError& ex) {
		return ex.what();
	}
	return "";
}

}

TEST(fetch_json_roundtrip, "fetch: JSON の往復と、2xx 以外の扱い (本文は 500 バイトまで)") {
	LoopThread lt;
	HttpServerOptions options;
	options.routes = {
		{ "POST", "/api", [](RequestContext& ctx) -> Reply {
			return Json::object({ { "echo", ctx.query.at("msg") }, { "type", ctx.headers.get("content-type") }, { "auth", ctx.headers.get("authorization") } });
		} },
		{ "GET", "/empty", [](RequestContext&) -> Reply { return Reply::no_content(); } },
		{ "GET", "/fail", [](RequestContext&) -> Reply { return text(std::string(2000, 'e'), 502); } },
	};
	HttpServer server(lt.loop(), std::move(options));
	const std::string base = "http://127.0.0.1:" + std::to_string(server.listen(0));

	JsonRequest req;
	req.headers = { { "Authorization", "Bearer t" } };
	req.body = Json::object({ { "msg", "こんにちは" } });
	const Json res = fetch_json(base + "/api", req);
	CHECK_EQ(res.at("echo").as_string(), "こんにちは");
	CHECK_EQ(res.at("type").as_string(), "application/json");
	CHECK_EQ(res.at("auth").as_string(), "Bearer t");
	CHECK(fetch_json(base + "/empty").is_null());

	bool caught = false;
	try {
		fetch_json(base + "/fail");
	} catch (const HttpError& ex) {
		caught = ex.status == 502 && ex.body.size() == 500;
	}
	CHECK(caught);
	server.close();
}

TEST(fetch_framing, "fetch: chunked / 長さ指定なしの応答を読み、上限超過と途中切断は失敗にする") {
	{
		CannedServer s("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n");
		CHECK_EQ(fetch(s.url()).body, "hello world");
	}
	{
		CannedServer s("HTTP/1.1 200 OK\r\n\r\nuntil close");
		CHECK_EQ(fetch(s.url()).body, "until close");
	}
	{
		CannedServer s("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok");
		const FetchResponse r = fetch(s.url());
		CHECK_EQ(r.status, 201);
		CHECK_EQ(r.body, "ok");
	}
	{
		CannedServer s("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
		CHECK_EQ(error_of(s.url()), "response truncated");
	}
	{
		CannedServer s("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n" + std::string(5000, 'x'));
		FetchRequest req;
		req.max_response_bytes = 1000;
		CHECK_EQ(error_of(s.url(), req), "response too large");
	}
	{
		CannedServer s("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
		CHECK_EQ(error_of(s.url()), "invalid chunk");
	}
}

TEST(fetch_no_redirect, "fetch: リダイレクトには従わない") {
	CannedServer s("HTTP/1.1 302 Found\r\nLocation: http://169.254.169.254/latest/meta-data/\r\nContent-Length: 0\r\n\r\n");
	CHECK_EQ(error_of(s.url()), "redirect not allowed (302)");
}

TEST(fetch_timeout, "fetch: 応答が来なければ時間で打ち切る") {
	CannedServer s("", false);
	FetchRequest req;
	req.timeout_ms = 200;
	const auto started = std::chrono::steady_clock::now();
	CHECK_EQ(error_of(s.url(), req), "timeout");
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(ms >= 150 && ms < 2000);
}

TEST(fetch_invalid_input, "fetch: 不正な URL・ヘッダ・パス要素を送る前に弾く") {
	CHECK_THROWS(fetch("ftp://example.com/"));
	CHECK_THROWS(fetch("http://user:pw@example.com/"));
	FetchRequest req;
	req.headers = { { "X-A", "a\r\nX-Injected: 1" } };
	CHECK_THROWS(fetch("http://127.0.0.1:1/", req));
	CHECK_EQ(path_segment("gemini-1.5-pro", "model"), "gemini-1.5-pro");
	CHECK_THROWS(path_segment("../x", "model"));
	CHECK_THROWS(path_segment("", "model"));
}

namespace {

//自己署名証明書 (SAN: localhost) を持つ TLS サーバ。接続ごとに 1 応答を返す
struct TlsServer {
	Listener listener;
	std::thread thread;
	std::atomic<bool> stop{false};
	SSL_CTX* ctx = nullptr;
	std::filesystem::path ca_file;

	TlsServer() : listener(listen_tcp("127.0.0.1", 0)) {
		EVP_PKEY* key = EVP_EC_gen("P-256");
		X509* cert = X509_new();
		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_set_pubkey(cert, key);
		X509_NAME* name = X509_get_subject_name(cert);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
		X509_set_issuer_name(cert, name);
		X509V3_CTX v3;
		X509V3_set_ctx_nodb(&v3);
		X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
		X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, "DNS:localhost");
		X509_add_ext(cert, san, -1);
		X509_EXTENSION_free(san);
		X509_sign(cert, key, EVP_sha256());

		ca_file = std::filesystem::temp_directory_path() / ("portus-ca-" + uuidv4() + ".pem");
		std::FILE* f = std::fopen(ca_file.string().c_str(), "wb");
		PEM_write_X509(f, cert);
		std::fclose(f);

		ctx = SSL_CTX_new(TLS_server_method());
		SSL_CTX_use_certificate(ctx, cert);
		SSL_CTX_use_PrivateKey(ctx, key);
		X509_free(cert);
		EVP_PKEY_free(key);
		thread = std::thread([this] { serve(); });
	}
	~TlsServer() {
		stop = true;
		thread.join();
		close_socket(listener.fd);
		SSL_CTX_free(ctx);
		std::filesystem::remove(ca_file);
	}

	void serve() {
		while (!stop) {
			if (!wait_socket(listener.fd, kRead, 20)) continue;
			const socket_t fd = accept_tcp(listener.fd, nullptr);
			if (fd == kInvalidSocket) continue;
			SSL* ssl = SSL_new(ctx);
			SSL_set_fd(ssl, static_cast<int>(fd));

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			const auto pump = [&](const std::function<int()>& step) {
				while (std::chrono::steady_clock::now() < deadline) {
					const int r = step();
					if (r > 0) return r;
					const int err = SSL_get_error(ssl, r);
					if (err == SSL_ERROR_WANT_READ) wait_socket(fd, kRead, 50);
					else if (err == SSL_ERROR_WANT_WRITE) wait_socket(fd, kWrite, 50);
					else return -1;
				}
				return -1;
			};
			char buf[4096];
			const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecure";
			if (pump([&] { return SSL_accept(ssl); }) > 0 && pump([&] { return SSL_read(ssl, buf, sizeof(buf)); }) > 0) {
				pump([&] { return SSL_write(ssl, response, static_cast<int>(std::strlen(response))); });
				SSL_shutdown(ssl);
			}
			SSL_free(ssl);
			close_socket(fd);
		}
	}
};

}

TEST(fetch_tls, "fetch: https は証明書の連鎖とホスト名を検証する") {
	TlsServer server;
	const std::string port = std::to_string(server.listener.port);

	FetchRequest trusted;
	trusted.ca_file = server.ca_file.string();
	trusted.timeout_ms = 5000;
	const FetchResponse ok = fetch("https://localhost:" + port + "/", trusted);
	CHECK_EQ(ok.status, 200);
	CHECK_EQ(ok.body, "secure");

	//信頼していない発行元 (既定の CA ストアには自己署名証明書が無い)
	FetchRequest untrusted;
	untrusted.timeout_ms = 5000;
	CHECK(error_of("https://localhost:" + port + "/", untrusted).find("certificate verification failed") != std::string::npos);

	//発行元は信頼しているが、証明書の名前が接続先と違う
	CHECK(error_of("https://127.0.0.1:" + port + "/", trusted).find("certificate verification failed") != std::string::npos);

	FetchRequest missing;
	missing.ca_file = "/no/such/ca.pem";
	CHECK(error_of("https://localhost:" + port + "/", missing).find("cannot load CA") != std::string::npos);
}
