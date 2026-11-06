import { LogLevel, isEnabled } from "./level.js"
import { LogRecord, redact } from "./record.js"
import { LogSink } from "./sink.js"

//出力先とレベルは全ロガーで共有する
interface LoggerShared {
	threshold: LogLevel;
	sinks: LogSink[];
}

//構造化ロガー。child() でスコープと固定フィールドを足せる
export class Logger {
	constructor(private shared: LoggerShared, private scope: string, private base: Record<string, any> = {}) {}

	child(scope: string, fields: Record<string, any> = {}): Logger {
		return new Logger(this.shared, `${this.scope}.${scope}`, { ...this.base, ...fields });
	}

	setLevel(level: LogLevel) { this.shared.threshold = level; }
	setSinks(sinks: LogSink[]) { this.shared.sinks = sinks; }

	debug(msg: string, fields?: Record<string, any>) { this.log("debug", msg, fields); }
	info(msg: string, fields?: Record<string, any>) { this.log("info", msg, fields); }
	warn(msg: string, fields?: Record<string, any>) { this.log("warn", msg, fields); }
	error(msg: string, fields?: Record<string, any>) { this.log("error", msg, fields); }

	log(level: LogLevel, msg: string, fields?: Record<string, any>) {
		if (!isEnabled(level, this.shared.threshold)) return;

		const rec: LogRecord = {
			time: new Date().toISOString(),
			level,
			scope: this.scope,
			msg,
			fields: redact({ ...this.base, ...(fields || {}) }),
		};
		for (const sink of this.shared.sinks) {
			try {
				sink.write(rec);
			} catch (ex) {
				//ログ出力の失敗で処理を止めない
			}
		}
	}
}

export function createRootLogger(scope: string, threshold: LogLevel, sinks: LogSink[]): Logger {
	return new Logger({ threshold, sinks }, scope);
}
