#pragma once
//構造化ロガー (レベル / JSON Lines / 日別ファイル / 秘匿キーのマスク)。どのスレッドからでも書ける
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "portus/json.hpp"

namespace portus {

enum class LogLevel { Debug = 10, Info = 20, Warn = 30, Error = 40 };

LogLevel parse_level(std::string_view text, LogLevel fallback);
const char* level_name(LogLevel level);

//1 行分のログ
struct LogRecord {
	std::string time;
	LogLevel level = LogLevel::Info;
	std::string scope;
	std::string msg;
	Json fields;	//オブジェクト
};

//秘匿キー (session / token / password / authorization / cookie など) をマスクしたコピーを返す。深さも制限する
Json redact(const Json& value, int depth = 0);

std::string format_json(const LogRecord& rec);		//JSON Lines
std::string format_pretty(const LogRecord& rec);	//時刻 レベル [scope] msg key=value ... (改行などの制御文字はエスケープ)

//ログの出力先
class LogSink {
public:
	virtual ~LogSink() = default;
	virtual void write(const LogRecord& rec) = 0;
	virtual void flush() {}
};
using SinkList = std::vector<std::shared_ptr<LogSink>>;

//標準出力 / 標準エラー (warn 以上)
class ConsoleSink : public LogSink {
public:
	explicit ConsoleSink(bool json) : json_(json) {}
	void write(const LogRecord& rec) override;
	void flush() override;

private:
	bool json_;
};

//<dir>/<prefix>-YYYY-MM-DD.log へ JSON Lines で追記する。開けない・書けないときは捨てる (サービスは止めない)
class DailyFileSink : public LogSink {
public:
	explicit DailyFileSink(std::string dir, std::string prefix = "app");
	~DailyFileSink() override;
	void write(const LogRecord& rec) override;
	void flush() override;

private:
	std::string dir_;
	std::string prefix_;
	std::string day_;
	void* file_ = nullptr;
};

//書き込みを専用スレッドへ逃がす。呼び出し側 (イベントループやハンドラ) を出力待ちで止めない
//待ち行列が max_queue を超えた分は捨て、後で捨てた件数を 1 行書く
class AsyncSink : public LogSink {
public:
	explicit AsyncSink(SinkList sinks, std::size_t max_queue = 8192);
	~AsyncSink() override;	//残りを書き切ってから終わる
	void write(const LogRecord& rec) override;
	void flush() override;	//積まれた分が書き終わるまで待つ

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

//child() でスコープと固定フィールドを足せる。出力先とレベルは親子で共有する
class Logger {
public:
	Logger();	//出力先なし
	Logger(std::string scope, LogLevel threshold, SinkList sinks);

	Logger child(const std::string& scope, const Json& fields = Json::object()) const;
	void set_level(LogLevel level);
	void set_sinks(SinkList sinks);
	void flush();

	void debug(std::string_view msg, const Json& fields = Json::object()) const { log(LogLevel::Debug, msg, fields); }
	void info(std::string_view msg, const Json& fields = Json::object()) const { log(LogLevel::Info, msg, fields); }
	void warn(std::string_view msg, const Json& fields = Json::object()) const { log(LogLevel::Warn, msg, fields); }
	void error(std::string_view msg, const Json& fields = Json::object()) const { log(LogLevel::Error, msg, fields); }
	void log(LogLevel level, std::string_view msg, const Json& fields = Json::object()) const;

private:
	struct Shared;
	std::shared_ptr<Shared> shared_;
	std::string scope_;
	Json base_;
};

struct LoggerOptions {
	std::string scope;
	LogLevel level = LogLevel::Info;
	bool json = false;			//true で JSON Lines、false で人が読む形式
	std::string dir;			//指定するとそのディレクトリへ日別ファイルも書く
	std::string file_prefix;	//既定は scope
	bool async = true;			//専用スレッドで書く
};
Logger create_logger(const LoggerOptions& options);

//環境変数から作る: LOG_LEVEL (debug|info|warn|error) / LOG_FORMAT (pretty|json) / LOG_DIR
Logger logger_from_env(const std::string& scope);

//捕捉されなかった例外をログに残してから終了する
void install_terminate_handler(const Logger& log);

}
