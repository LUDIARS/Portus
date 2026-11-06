import { createRootLogger, Logger } from "./logger.js"
import { LogLevel, parseLevel } from "./level.js"
import { ConsoleSink, DailyFileSink, LogSink } from "./sink.js"

export { Logger, createRootLogger } from "./logger.js"
export { parseLevel } from "./level.js"
export type { LogLevel } from "./level.js"
export { redact } from "./record.js"
export type { LogRecord } from "./record.js"
export { ConsoleSink, DailyFileSink } from "./sink.js"
export type { LogSink } from "./sink.js"
export { installConsoleBridge } from "./consoleBridge.js"
export { installProcessHandlers } from "./process.js"

export interface LoggerOptions {
	scope: string;
	level?: LogLevel;
	json?: boolean;		//true で JSON Lines、false で人が読む形式
	dir?: string;		//指定するとそのディレクトリへ日別ファイルも書く
	filePrefix?: string;
}

export function createLogger(options: LoggerOptions): Logger {
	const sinks: LogSink[] = [new ConsoleSink(options.json ?? false)];
	if (options.dir) sinks.push(new DailyFileSink(options.dir, options.filePrefix ?? options.scope));
	return createRootLogger(options.scope, options.level ?? "info", sinks);
}

//環境変数から作る
//  LOG_LEVEL  = debug | info | warn | error (既定 info)
//  LOG_FORMAT = pretty | json (既定 pretty)
//  LOG_DIR    = 指定時はそのディレクトリへ日別ファイルも書く
export function loggerFromEnv(scope: string, env: NodeJS.ProcessEnv = process.env): Logger {
	return createLogger({
		scope,
		level: parseLevel(env.LOG_LEVEL, "info"),
		json: env.LOG_FORMAT == "json",
		dir: env.LOG_DIR || undefined,
	});
}
