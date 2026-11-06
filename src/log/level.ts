//ログレベル定義
export type LogLevel = "debug" | "info" | "warn" | "error";

const ORDER: Record<LogLevel, number> = { debug: 10, info: 20, warn: 30, error: 40 };

export function parseLevel(text: string | undefined, fallback: LogLevel): LogLevel {
	const v = (text || "").toLowerCase();
	return (v in ORDER) ? v as LogLevel : fallback;
}

export function isEnabled(level: LogLevel, threshold: LogLevel): boolean {
	return ORDER[level] >= ORDER[threshold];
}
