import * as fs from "node:fs"
import * as path from "node:path"
import { LogRecord } from "./record.js"
import { formatJson, formatPretty } from "./format.js"

//ログの出力先
export interface LogSink {
	write(rec: LogRecord): void;
}

//標準出力 / 標準エラー(warn 以上)へ書く。console を経由しないので console ブリッジと循環しない
export class ConsoleSink implements LogSink {
	constructor(private json: boolean) {}

	write(rec: LogRecord) {
		const line = (this.json ? formatJson(rec) : formatPretty(rec)) + "\n";
		if (rec.level == "warn" || rec.level == "error") process.stderr.write(line);
		else process.stdout.write(line);
	}
}

//日付ごとのファイルへ JSON Lines で追記する
export class DailyFileSink implements LogSink {
	private day: string = "";
	private stream: fs.WriteStream | null = null;

	constructor(private dir: string, private prefix: string = "app") {
		fs.mkdirSync(dir, { recursive: true });
	}

	write(rec: LogRecord) {
		const day = rec.time.substring(0, 10);
		if (day != this.day || !this.stream) {
			this.stream?.end();
			this.day = day;
			this.stream = fs.createWriteStream(path.join(this.dir, `${this.prefix}-${day}.log`), { flags: "a" });
		}
		this.stream.write(formatJson(rec) + "\n");
	}
}
