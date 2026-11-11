#include "portus/log.hpp"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>

#include "portus/env.hpp"
#include "portus/util.hpp"

namespace portus {

struct Logger::Shared {
	std::atomic<int> threshold{static_cast<int>(LogLevel::Info)};
	std::mutex mu;
	SinkList sinks;
};

Logger::Logger() : shared_(std::make_shared<Shared>()), base_(Json::object()) {}

Logger::Logger(std::string scope, LogLevel threshold, SinkList sinks) : shared_(std::make_shared<Shared>()), scope_(std::move(scope)), base_(Json::object()) {
	shared_->threshold.store(static_cast<int>(threshold));
	shared_->sinks = std::move(sinks);
}

Logger Logger::child(const std::string& scope, const Json& fields) const {
	Logger out = *this;
	out.scope_ = scope_.empty() ? scope : scope_ + "." + scope;
	for (const Json::Member& m : fields.members()) out.base_.set(m.first, m.second);
	return out;
}

void Logger::set_level(LogLevel level) {
	shared_->threshold.store(static_cast<int>(level));
}

void Logger::set_sinks(SinkList sinks) {
	std::lock_guard<std::mutex> lock(shared_->mu);
	shared_->sinks = std::move(sinks);
}

void Logger::flush() {
	std::lock_guard<std::mutex> lock(shared_->mu);
	for (const auto& sink : shared_->sinks) sink->flush();
}

void Logger::log(LogLevel level, std::string_view msg, const Json& fields) const {
	if (static_cast<int>(level) < shared_->threshold.load()) return;

	Json merged = base_;
	for (const Json::Member& m : fields.members()) merged.set(m.first, m.second);

	LogRecord rec;
	rec.time = iso_time(now_ms());
	rec.level = level;
	rec.scope = scope_;
	rec.msg = std::string(msg);
	rec.fields = redact(merged);

	std::lock_guard<std::mutex> lock(shared_->mu);
	for (const auto& sink : shared_->sinks) {
		try {
			sink->write(rec);
		} catch (...) {
			//ログ出力の失敗で処理を止めない
		}
	}
}

Logger create_logger(const LoggerOptions& options) {
	SinkList sinks;
	sinks.push_back(std::make_shared<ConsoleSink>(options.json));
	if (!options.dir.empty()) {
		sinks.push_back(std::make_shared<DailyFileSink>(options.dir, options.file_prefix.empty() ? options.scope : options.file_prefix));
	}
	if (options.async) {
		SinkList wrapped;
		wrapped.push_back(std::make_shared<AsyncSink>(std::move(sinks)));
		sinks = std::move(wrapped);
	}
	return Logger(options.scope, options.level, std::move(sinks));
}

Logger logger_from_env(const std::string& scope) {
	LoggerOptions options;
	options.scope = scope;
	options.level = parse_level(get_env("LOG_LEVEL"), LogLevel::Info);
	options.json = get_env("LOG_FORMAT") == "json";
	options.dir = get_env("LOG_DIR");
	return create_logger(options);
}

void install_terminate_handler(const Logger& log) {
	static Logger target;
	target = log;
	std::set_terminate([] {
		std::string what = "unknown";
		try {
			if (std::exception_ptr ex = std::current_exception()) std::rethrow_exception(ex);
		} catch (const std::exception& e) {
			what = e.what();
		} catch (...) {
		}
		target.error("uncaught exception", Json::object({ { "err", Json(what) } }));
		target.flush();
		std::abort();
	});
}

}
