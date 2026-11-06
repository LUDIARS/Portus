//Redis RESP2 プロトコル (コマンド送信と応答の解析)
const CRLF = "\r\n";
const MAX_BULK = 64 * 1024 * 1024;
const MAX_ARRAY = 1024 * 1024;

export class RedisReplyError extends Error {}

//コマンドを RESP 配列にする
export function encodeCommand(args: string[]): Buffer {
	const parts: Buffer[] = [Buffer.from(`*${args.length}${CRLF}`)];
	for (const arg of args) {
		const b = Buffer.from(arg, "utf8");
		parts.push(Buffer.from(`$${b.length}${CRLF}`), b, Buffer.from(CRLF));
	}
	return Buffer.concat(parts);
}

const INCOMPLETE = Symbol("incomplete");

//受信バイト列から応答を順に取り出す
export class RespParser {
	private buffer: Buffer = Buffer.alloc(0);

	push(chunk: Buffer): any[] {
		this.buffer = this.buffer.length == 0 ? chunk : Buffer.concat([this.buffer, chunk]);
		const replies: any[] = [];
		for (;;) {
			const r = this.parse(0);
			if (r === INCOMPLETE) break;
			const [value, next] = r as [any, number];
			this.buffer = this.buffer.subarray(next);
			replies.push(value);
		}
		return replies;
	}

	private readLine(offset: number): [string, number] | typeof INCOMPLETE {
		const end = this.buffer.indexOf(CRLF, offset);
		if (end < 0) return INCOMPLETE;
		return [this.buffer.toString("utf8", offset, end), end + 2];
	}

	private parse(offset: number): [any, number] | typeof INCOMPLETE {
		if (this.buffer.length <= offset) return INCOMPLETE;
		const type = String.fromCharCode(this.buffer[offset]!);
		const line = this.readLine(offset + 1);
		if (line === INCOMPLETE) return INCOMPLETE;
		const [text, next] = line;

		switch (type) {
		case "+": return [text, next];
		case "-": return [new RedisReplyError(text), next];
		case ":": return [Number(text), next];
		case "$": {
			const len = Number(text);
			if (len < 0) return [null, next];
			if (!Number.isInteger(len) || len > MAX_BULK) throw new Error("invalid bulk length");
			if (this.buffer.length < next + len + 2) return INCOMPLETE;
			return [this.buffer.toString("utf8", next, next + len), next + len + 2];
		}
		case "*": {
			const count = Number(text);
			if (count < 0) return [null, next];
			if (!Number.isInteger(count) || count > MAX_ARRAY) throw new Error("invalid array length");
			const items: any[] = [];
			let pos = next;
			for (let i = 0; i < count; ++i) {
				const r = this.parse(pos);
				if (r === INCOMPLETE) return INCOMPLETE;
				items.push(r[0]);
				pos = r[1];
			}
			return [items, pos];
		}
		}
		throw new Error(`unknown RESP type ${type}`);
	}
}
