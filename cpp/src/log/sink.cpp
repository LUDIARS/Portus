#include "portus/log.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace portus {

void ConsoleSink::write(const LogRecord& rec) {
	const std::string line = (json_ ? format_json(rec) : format_pretty(rec)) + "\n";
	std::fwrite(line.data(), 1, line.size(), rec.level >= LogLevel::Warn ? stderr : stdout);
}

void ConsoleSink::flush() {
	std::fflush(stdout);
	std::fflush(stderr);
}

DailyFileSink::DailyFileSink(std::string dir, std::string prefix) : dir_(std::move(dir)), prefix_(std::move(prefix)) {
	std::error_code ec;
	std::filesystem::create_directories(dir_, ec);
}

DailyFileSink::~DailyFileSink() {
	if (file_) std::fclose(static_cast<std::FILE*>(file_));
}

void DailyFileSink::write(const LogRecord& rec) {
	const std::string day = rec.time.substr(0, 10);
	if (day != day_ || !file_) {
		if (file_) std::fclose(static_cast<std::FILE*>(file_));
		day_ = day;
		const std::filesystem::path path = std::filesystem::path(dir_) / (prefix_ + "-" + day + ".log");
		file_ = std::fopen(path.string().c_str(), "ab");
	}
	if (!file_) return;

	const std::string line = format_json(rec) + "\n";
	std::fwrite(line.data(), 1, line.size(), static_cast<std::FILE*>(file_));
}

void DailyFileSink::flush() {
	if (file_) std::fflush(static_cast<std::FILE*>(file_));
}

}
