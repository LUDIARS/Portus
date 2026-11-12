#include "portus/log.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <share.h>
#endif

namespace portus {

void ConsoleSink::write(const LogRecord& rec) {
	const std::string line = (json_ ? format_json(rec) : format_pretty(rec)) + "\n";
	std::fwrite(line.data(), 1, line.size(), rec.level >= LogLevel::Warn ? stderr : stdout);
}

void ConsoleSink::flush() {
	std::fflush(stdout);
	std::fflush(stderr);
}

namespace {

//dir / prefix は UTF-8 として扱う。Windows の std::string からのパスは ANSI コードページとして解釈されるので、
//  日本語のパスが別の名前になったり、path::string() が例外を投げたりする。u8path でワイド文字のパスにする
std::filesystem::path utf8_path(const std::string& text) {
	return std::filesystem::u8path(text);
}

std::FILE* open_append(const std::filesystem::path& path) {
#ifdef _WIN32
	//ワイド文字のまま開く (narrow の fopen は ANSI コードページ)。_wfopen_s は排他で開いて他から読めなくなるので、共有を明示する
	return _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
	return std::fopen(path.c_str(), "ab");
#endif
}

}

DailyFileSink::DailyFileSink(std::string dir, std::string prefix) : dir_(std::move(dir)), prefix_(std::move(prefix)) {
	//作れなくても落とさない (開けなければ write で捨てる)。UTF-8 として読めない名前の例外もここで止める
	try {
		std::error_code ec;
		std::filesystem::create_directories(utf8_path(dir_), ec);
	} catch (const std::exception&) {
	}
}

DailyFileSink::~DailyFileSink() {
	if (file_) std::fclose(static_cast<std::FILE*>(file_));
}

void DailyFileSink::write(const LogRecord& rec) {
	const std::string day = rec.time.substr(0, 10);
	if (day != day_ || !file_) {
		if (file_) std::fclose(static_cast<std::FILE*>(file_));
		file_ = nullptr;
		day_ = day;
		try {
			file_ = open_append(utf8_path(dir_) / utf8_path(prefix_ + "-" + day + ".log"));
		} catch (const std::exception&) {
		}
	}
	if (!file_) return;

	const std::string line = format_json(rec) + "\n";
	std::fwrite(line.data(), 1, line.size(), static_cast<std::FILE*>(file_));
}

void DailyFileSink::flush() {
	if (file_) std::fflush(static_cast<std::FILE*>(file_));
}

}
