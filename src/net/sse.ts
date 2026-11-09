//Server-Sent Events の解析。区切りは空行。改行は CRLF / LF / CR のどれでもよい (チャンクの境界で CR と LF が分かれても同じ)
export interface SseEvent {
	event: string;	//event: が無ければ ""
	data: string;	//複数行の data は "\n" でつなぐ
	id?: string;
}

const DEFAULT_MAX_EVENT_BYTES = 1024 * 1024;

export async function* parseSse(body: AsyncIterable<Uint8Array>, maxEventBytes: number = DEFAULT_MAX_EVENT_BYTES): AsyncGenerator<SseEvent, void, unknown> {
	const decoder = new TextDecoder();
	const newlines = newlineNormalizer();
	let buffer = "";
	for await (const chunk of body) {
		buffer += newlines.push(decoder.decode(chunk, { stream: true }));
		let end: number;
		while ((end = buffer.indexOf("\n\n")) >= 0) {
			const event = parseBlock(buffer.substring(0, end));
			buffer = buffer.substring(end + 2);
			if (event) yield event;
		}
		//区切りの来ないまま膨らむ入力は打ち切る (未消化分だけを数える)
		if (buffer.length > maxEventBytes) throw new Error("server-sent event exceeds the size limit");
	}
	buffer += newlines.push(decoder.decode()) + newlines.end();
	const event = parseBlock(buffer);
	if (event) yield event;
}

function parseBlock(block: string): SseEvent | null {
	let event = "";
	let id: string | undefined;
	const data: string[] = [];
	for (const line of block.split("\n")) {
		if (line == "" || line.startsWith(":")) continue;
		const colon = line.indexOf(":");
		const field = colon < 0 ? line : line.substring(0, colon);
		const value = colon < 0 ? "" : line.substring(colon + 1).replace(/^ /, "");
		if (field == "event") event = value;
		else if (field == "data") data.push(value);
		else if (field == "id") id = value;
	}
	if (data.length == 0) return null;
	return id === undefined ? { event, data: data.join("\n") } : { event, data: data.join("\n"), id };
}

//\r\n と単独の \r を \n にそろえる。チャンク末尾の \r は次のチャンクの \n と組になりうるので持ち越す
function newlineNormalizer() {
	let pendingCr = false;
	return {
		push(text: string): string {
			let s = pendingCr ? "\r" + text : text;
			pendingCr = s.endsWith("\r");
			if (pendingCr) s = s.substring(0, s.length - 1);
			return s.replace(/\r\n?/g, "\n");
		},
		end(): string {
			const tail = pendingCr ? "\n" : "";
			pendingCr = false;
			return tail;
		},
	};
}
