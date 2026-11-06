import { LogLevel } from "./level.js"

//1 行分のログ
export interface LogRecord {
	time: string;
	level: LogLevel;
	scope: string;
	msg: string;
	fields: Record<string, any>;
}

//ログに残してはいけない値を持つキー(小文字で比較)
const SECRET_KEYS = new Set(["session", "token", "password", "pass", "auth", "authorization", "p256dh", "secret", "apikey", "api_key", "key"]);

//秘匿キーをマスクしたコピーを返す。循環参照と深さも制限する
export function redact(value: any, depth: number = 0, seen: WeakSet<object> = new WeakSet()): any {
	if (value == null || typeof value != "object") return value;
	if (value instanceof Error) return { name: value.name, message: value.message, stack: value.stack };
	if (seen.has(value)) return "[Circular]";
	if (depth >= 6) return "[Depth]";
	seen.add(value);

	if (Array.isArray(value)) return value.map((v) => redact(v, depth + 1, seen));
	if (Buffer.isBuffer(value)) return `[Buffer ${value.length}]`;

	const out: Record<string, any> = {};
	for (const k of Object.keys(value)) {
		out[k] = SECRET_KEYS.has(k.toLowerCase()) ? "[REDACTED]" : redact(value[k], depth + 1, seen);
	}
	return out;
}
