import { LogRecord } from "./record.js"

//JSON Lines 形式
export function formatJson(rec: LogRecord): string {
	return JSON.stringify({ time: rec.time, level: rec.level, scope: rec.scope, msg: rec.msg, ...rec.fields });
}

//人が読む形式: 時刻 レベル [scope] msg key=value ...
export function formatPretty(rec: LogRecord): string {
	const parts = [rec.time, rec.level.toUpperCase().padEnd(5), `[${rec.scope}]`, rec.msg];
	for (const k of Object.keys(rec.fields)) {
		const v = rec.fields[k];
		parts.push(`${k}=${typeof v == "string" ? v : JSON.stringify(v)}`);
	}
	return parts.join(" ");
}
