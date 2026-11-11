#include "channel.hpp"

#include <mutex>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "portus/net.hpp"

namespace portus {

namespace {

SSL_CTX* new_context(const std::string& ca_file) {
	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx) throw FetchError("tls: cannot create context");
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
	const int ok = ca_file.empty() ? SSL_CTX_set_default_verify_paths(ctx) : SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr);
	if (ok != 1) {
		SSL_CTX_free(ctx);
		throw FetchError("tls: cannot load CA certificates");
	}
	return ctx;
}

//既定の CA を使うコンテキストは読み込みが重いので使い回す
SSL_CTX* default_context() {
	static std::once_flag once;
	static SSL_CTX* ctx = nullptr;
	std::call_once(once, [] { ctx = new_context(""); });
	return ctx;
}

std::string tls_error(const char* what) {
	char buf[256] = "";
	const unsigned long code = ERR_get_error();
	if (code) ERR_error_string_n(code, buf, sizeof(buf));
	ERR_clear_error();
	return std::string("tls: ") + what + (code ? std::string(": ") + buf : std::string());
}

}

Channel::Channel(const std::string& host, int port, bool tls, const std::string& ca_file, Deadline deadline) : deadline_(deadline) {
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
	if (left <= 0) throw FetchError("timeout");
	try {
		fd_ = connect_tcp(host, port, static_cast<int>(left));
	} catch (const std::exception& ex) {
		throw FetchError(ex.what());
	}
	if (!tls) return;

	try {
		SSL_CTX* ctx = default_context();
		if (!ca_file.empty()) {
			ctx = new_context(ca_file);
			owned_ctx_ = ctx;
		}
		SSL* ssl = SSL_new(ctx);
		if (!ssl) throw FetchError("tls: cannot create session");
		ssl_ = ssl;

		//証明書の名前が接続先と一致することまで検証する
		if (SSL_set_fd(ssl, static_cast<int>(fd_)) != 1 || SSL_set_tlsext_host_name(ssl, host.c_str()) != 1 || SSL_set1_host(ssl, host.c_str()) != 1) {
			throw FetchError("tls: cannot configure session");
		}
		for (;;) {
			ERR_clear_error();
			const int r = SSL_connect(ssl);
			if (r == 1) break;
			const int err = SSL_get_error(ssl, r);
			if (err == SSL_ERROR_WANT_READ) wait(kRead);
			else if (err == SSL_ERROR_WANT_WRITE) wait(kWrite);
			else if (SSL_get_verify_result(ssl) != X509_V_OK) throw FetchError(std::string("tls: certificate verification failed: ") + X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
			else throw FetchError(tls_error("handshake failed"));
		}
	} catch (...) {
		if (ssl_) SSL_free(static_cast<SSL*>(ssl_));
		if (owned_ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(owned_ctx_));
		close_socket(fd_);
		throw;
	}
}

Channel::~Channel() {
	if (ssl_) {
		SSL_shutdown(static_cast<SSL*>(ssl_));	//close_notify を送るだけ。返事は待たない
		SSL_free(static_cast<SSL*>(ssl_));
	}
	if (owned_ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(owned_ctx_));
	close_socket(fd_);
}

void Channel::wait(unsigned interest) {
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now()).count();
	if (left <= 0 || wait_socket(fd_, interest, static_cast<int>(left)) == 0) throw FetchError("timeout");
}

void Channel::write_all(const std::string& data) {
	std::size_t sent = 0;
	while (sent < data.size()) {
		if (ssl_) {
			ERR_clear_error();
			const int n = SSL_write(static_cast<SSL*>(ssl_), data.data() + sent, static_cast<int>(data.size() - sent));
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
				continue;
			}
			const int err = SSL_get_error(static_cast<SSL*>(ssl_), n);
			if (err == SSL_ERROR_WANT_READ) wait(kRead);
			else if (err == SSL_ERROR_WANT_WRITE) wait(kWrite);
			else throw FetchError(tls_error("write failed"));
		} else {
			const long n = socket_send(fd_, data.data() + sent, data.size() - sent);
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
				continue;
			}
			if (!last_error_would_block()) throw FetchError("connection closed while sending");
			wait(kWrite);
		}
	}
}

std::size_t Channel::read(char* buf, std::size_t len) {
	for (;;) {
		if (ssl_) {
			ERR_clear_error();
			const int n = SSL_read(static_cast<SSL*>(ssl_), buf, static_cast<int>(len));
			if (n > 0) return static_cast<std::size_t>(n);
			const int err = SSL_get_error(static_cast<SSL*>(ssl_), n);
			if (err == SSL_ERROR_ZERO_RETURN) return 0;
			if (err == SSL_ERROR_WANT_READ) wait(kRead);
			else if (err == SSL_ERROR_WANT_WRITE) wait(kWrite);
			else throw FetchError(tls_error("read failed"));	//close_notify 無しの切断もここ (途中で切られた応答を完全なものとして扱わない)
		} else {
			const long n = socket_recv(fd_, buf, len);
			if (n >= 0) return static_cast<std::size_t>(n);
			if (!last_error_would_block()) throw FetchError("connection reset");
			wait(kRead);
		}
	}
}

}
