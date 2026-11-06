import * as fs from "node:fs"
import * as path from "node:path"

//dotenv の置き換え。KEY=VALUE 形式だけを扱う(変数展開・複数行値は扱わない)
const KEY_PATTERN = /^[A-Za-z_][A-Za-z0-9_]*$/;

//.env テキストを解析する
export function parseEnv(text: string): Record<string, string> {
	const result: Record<string, string> = Object.create(null);
	for (const rawLine of text.split(/\r?\n/)) {
		let line = rawLine.trim();
		if (line == "" || line.startsWith("#")) continue;
		if (line.startsWith("export ")) line = line.substring(7).trim();

		const eq = line.indexOf("=");
		if (eq <= 0) continue;

		const key = line.substring(0, eq).trim();
		if (!KEY_PATTERN.test(key)) continue;

		result[key] = parseValue(line.substring(eq + 1).trim());
	}
	return result;
}

function parseValue(value: string): string {
	const quote = value.charAt(0);
	if ((quote == '"' || quote == "'") && value.length >= 2) {
		const end = value.indexOf(quote, 1);
		if (end > 0) {
			const inner = value.substring(1, end);
			return quote == '"' ? inner.replace(/\\n/g, "\n") : inner;
		}
	}

	//クォート無しは行内コメント(空白 + #)を落とす
	const comment = value.search(/\s#/);
	return (comment >= 0 ? value.substring(0, comment) : value).trim();
}

//.env を読み込んで process.env に入れる(既に設定済みのキーは上書きしない)
export function loadEnv(file: string = path.resolve(process.cwd(), ".env")) {
	let text = "";
	try {
		text = fs.readFileSync(file, "utf8");
	} catch (ex) {
		return;
	}

	const values = parseEnv(text);
	for (const key of Object.keys(values)) {
		if (process.env[key] === undefined) process.env[key] = values[key];
	}
}
