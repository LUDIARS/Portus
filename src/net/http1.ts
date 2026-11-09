import { NetError } from "./errors.js"

//HTTP/1.1 の組み立てと応答の解析。曖昧な形は受けず、終わりの分かる本文 (Content-Length か chunked) だけを受ける。
//接続は毎回閉じる (Connection: close) ので、パイプライン・使い回しは扱わない

export type Method = "GET" | "POST" | "PUT";

const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const HEADER_VALUE = /^[\t\x20-\x7e]*$/;	//送る値は ASCII だけ (CR / LF / NUL を入れさせない)
const REQUEST_TARGET = /^\/[\x21-\x7e]*$/;
//通信の形はこの層が決めるので、呼び出し側からは付けさせない
const RESERVED_HEADERS = new Set(["host", "content-length", "transfer-encoding", "connection", "accept-encoding", "te", "trailer", "upgrade", "expect", "keep-alive", "proxy-connection"]);

export function serializeRequest(method: Method, url: URL, headers: Record<string, string>, body: Buffer | null): Buffer {
	if (method != "GET" && method != "POST" && method != "PUT") throw new Error(`unsupported method ${method}`);
	const target = url.pathname + url.search;
	if (!REQUEST_TARGET.test(target)) throw new Error("invalid request target");

	const lines = [`${method} ${target} HTTP/1.1`, `Host: ${url.host}`];
	const seen = new Set<string>();
	for (const [name, value] of Object.entries(headers)) {
		const lower = name.toLowerCase();
		if (!TOKEN.test(name)) throw new Error(`invalid header name ${JSON.stringify(name)}`);
		if (RESERVED_HEADERS.has(lower)) throw new Error(`header ${name} is set by the transport`);
		if (seen.has(lower)) throw new Error(`duplicate header ${name}`);
		if (typeof value != "string" || !HEADER_VALUE.test(value)) throw new Error(`invalid value for header ${name}`);
		seen.add(lower);
		lines.push(`${name}: ${value}`);
	}
	if (body !== null || method != "GET") lines.push(`Content-Length: ${body?.length ?? 0}`);
	lines.push("Accept-Encoding: identity", "Connection: close", "", "");
	return Buffer.from(lines.join("\r\n"), "latin1");
}

export interface ResponseHead {
	status: number;
	reason: string;
	headers: Record<string, string>;	//null プロトタイプ、名前は小文字、同名は ", " でつなぐ
}

export interface ParserLimits {
	maxHeaderBytes: number;		//状態行とヘッダ (中間応答・トレーラを含む) の合計
	maxHeaderCount: number;
	maxBodyBytes: number;
}

export type ParseEvent = { type: "head", head: ResponseHead } | { type: "data", chunk: Buffer } | { type: "end" };

const STATUS_LINE = /^HTTP\/1\.[01] ([1-9]\d{2})(?: ([^\x00-\x08\x0a-\x1f\x7f]*))?$/;
const CHUNK_SIZE = /^([0-9A-Fa-f]{1,8})(?:[ \t]*;[\t\x20-\x7e\x80-\xff]*)?$/;
const MAX_CHUNK_LINE = 1024;
const MAX_INTERIM_RESPONSES = 8;
const CRLF = Buffer.from("\r\n");
const HEAD_END = Buffer.from("\r\n\r\n");

type State = "head" | "length" | "chunkSize" | "chunkData" | "chunkDataEnd" | "trailers" | "done";

function protocolError(message: string): NetError {
	return new NetError("PROTOCOL", message);
}

export class ResponseParser {
	private state: State = "head";
	private pending: Buffer = Buffer.alloc(0);
	private headerBytes = 0;
	private headerCount = 0;
	private interim = 0;
	private remaining = 0;		//length / chunkData で残っている本文
	private bodyBytes = 0;

	constructor(private limits: ParserLimits) {}

	get done(): boolean {
		return this.state == "done";
	}

	push(data: Buffer): ParseEvent[] {
		const events: ParseEvent[] = [];
		this.pending = this.pending.length ? Buffer.concat([this.pending, data]) : data;
		while (this.state != "done") {
			if (!this.step(events)) break;
		}
		return events;
	}

	//接続が閉じた。応答が終わっていなければ切断として扱う
	finish() {
		if (this.state != "done") throw new NetError("CLOSED", "connection closed before the response completed");
	}

	//1 段進めたら true、データが足りなければ false
	private step(events: ParseEvent[]): boolean {
		switch (this.state) {
			case "head": return this.parseHead(events);
			case "length": {
				if (this.pending.length == 0) return false;
				const n = Math.min(this.remaining, this.pending.length);
				this.emitData(events, this.take(n));
				this.remaining -= n;
				if (this.remaining == 0) this.finishBody(events);
				return true;
			}
			case "chunkSize": {
				const line = this.takeLine(MAX_CHUNK_LINE, "chunk size line");
				if (line === null) return false;
				const m = CHUNK_SIZE.exec(line);
				if (!m) throw protocolError("invalid chunk size");
				const size = parseInt(m[1]!, 16);
				if (size == 0) {
					this.state = "trailers";
					return true;
				}
				if (this.bodyBytes + size > this.limits.maxBodyBytes) throw new NetError("LIMIT", "response body exceeds the limit");
				this.remaining = size;
				this.state = "chunkData";
				return true;
			}
			case "chunkData": {
				if (this.pending.length == 0) return false;
				const n = Math.min(this.remaining, this.pending.length);
				this.emitData(events, this.take(n));
				this.remaining -= n;
				if (this.remaining == 0) this.state = "chunkDataEnd";
				return true;
			}
			case "chunkDataEnd": {
				if (this.pending.length < 2) return false;
				if (!this.take(2).equals(CRLF)) throw protocolError("chunk data is not followed by CRLF");
				this.state = "chunkSize";
				return true;
			}
			case "trailers": {
				const line = this.takeLine(this.limits.maxHeaderBytes - this.headerBytes, "trailer");
				if (line === null) return false;
				if (line == "") {
					this.finishBody(events);
					return true;
				}
				this.headerBytes += line.length + 2;
				this.parseField(line);	//形だけ検査して捨てる
				return true;
			}
			default: return false;
		}
	}

	private parseHead(events: ParseEvent[]): boolean {
		const end = this.pending.indexOf(HEAD_END);
		if (end < 0) {
			if (this.headerBytes + this.pending.length > this.limits.maxHeaderBytes) throw new NetError("LIMIT", "response headers exceed the limit");
			return false;
		}
		this.headerBytes += end + 4;
		if (this.headerBytes > this.limits.maxHeaderBytes) throw new NetError("LIMIT", "response headers exceed the limit");
		const block = this.take(end + 4).toString("latin1", 0, end);
		const lines = block.split("\r\n");
		const status = STATUS_LINE.exec(lines[0]!);
		if (!status) throw protocolError("invalid status line");

		const headers: Record<string, string> = Object.create(null);
		const lengths: string[] = [];
		for (const line of lines.slice(1)) {
			if (++this.headerCount > this.limits.maxHeaderCount) throw new NetError("LIMIT", "too many response headers");
			const [name, value] = this.parseField(line);
			if (name == "content-length") lengths.push(value);
			headers[name] = headers[name] === undefined ? value : `${headers[name]}, ${value}`;
		}

		const code = Number(status[1]);
		if (code >= 100 && code < 200) {
			if (code == 101) throw protocolError("protocol upgrade is not supported");
			if (++this.interim > MAX_INTERIM_RESPONSES) throw protocolError("too many interim responses");
			return true;	//中間応答は読み捨てて次の応答を待つ
		}

		events.push({ type: "head", head: { status: code, reason: status[2] ?? "", headers } });
		this.startBody(code, headers, lengths, events);
		return true;
	}

	private startBody(status: number, headers: Record<string, string>, lengths: string[], events: ParseEvent[]) {
		const encoding = headers["content-encoding"];
		if (encoding !== undefined && encoding.trim().toLowerCase() != "identity") throw protocolError("compressed responses are not accepted");
		if (status == 204 || status == 304) {
			this.finishBody(events);
			return;
		}
		const transfer = headers["transfer-encoding"];
		if (transfer !== undefined) {
			if (lengths.length) throw protocolError("response has both Transfer-Encoding and Content-Length");
			if (transfer.trim().toLowerCase() != "chunked") throw protocolError(`unsupported transfer encoding ${transfer}`);
			this.state = "chunkSize";
			return;
		}
		if (lengths.length == 0) throw protocolError("response has no Content-Length or chunked encoding");
		//同じ値の重複 (5, 5) だけは許す
		const values = lengths.flatMap((v) => v.split(",").map((s) => s.trim()));
		if (!values.every((v) => /^\d{1,15}$/.test(v) && v == values[0])) throw protocolError("invalid Content-Length");
		const length = Number(values[0]);
		if (length > this.limits.maxBodyBytes) throw new NetError("LIMIT", "response body exceeds the limit");
		if (length == 0) {
			this.finishBody(events);
			return;
		}
		this.remaining = length;
		this.state = "length";
	}

	private parseField(line: string): [string, string] {
		if (line.startsWith(" ") || line.startsWith("\t")) throw protocolError("folded header lines are not accepted");
		const colon = line.indexOf(":");
		const name = colon > 0 ? line.substring(0, colon) : "";
		if (!TOKEN.test(name)) throw protocolError("invalid header name");
		const value = line.substring(colon + 1).replace(/^[ \t]+|[ \t]+$/g, "");
		if (/[\x00-\x08\x0a-\x1f\x7f]/.test(value)) throw protocolError("invalid header value");
		return [name.toLowerCase(), value];
	}

	private emitData(events: ParseEvent[], chunk: Buffer) {
		this.bodyBytes += chunk.length;
		if (this.bodyBytes > this.limits.maxBodyBytes) throw new NetError("LIMIT", "response body exceeds the limit");
		events.push({ type: "data", chunk });
	}

	private finishBody(events: ParseEvent[]) {
		this.state = "done";
		this.pending = Buffer.alloc(0);	//終わった後に届いたものは読まない (接続は閉じる)
		events.push({ type: "end" });
	}

	private take(n: number): Buffer {
		const out = this.pending.subarray(0, n);
		this.pending = this.pending.subarray(n);
		return out;
	}

	//CRLF までの 1 行。max を超えても行末が来なければ上限超過
	private takeLine(max: number, what: string): string | null {
		const end = this.pending.indexOf(CRLF);
		if (end < 0) {
			if (this.pending.length > max) throw new NetError("LIMIT", `${what} exceeds the limit`);
			return null;
		}
		if (end > max) throw new NetError("LIMIT", `${what} exceeds the limit`);
		const line = this.take(end + 2).toString("latin1", 0, end);
		if (line.includes("\n") || line.includes("\r")) throw protocolError(`invalid ${what}`);
		return line;
	}
}
