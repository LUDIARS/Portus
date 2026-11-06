import { ServerResponse } from "node:http"

//ステータス・本文・ヘッダを明示した応答
export class Reply {
	constructor(public status: number, public body: string | Buffer = "", public headers: Record<string, string> = {}) {}
}

export function json(data: any, status: number = 200, headers: Record<string, string> = {}): Reply {
	return new Reply(status, JSON.stringify(data), { "content-type": "application/json; charset=utf-8", ...headers });
}

export function text(body: string, status: number = 200, headers: Record<string, string> = {}): Reply {
	return new Reply(status, body, { "content-type": "text/plain; charset=utf-8", ...headers });
}

export function html(body: string, status: number = 200, headers: Record<string, string> = {}): Reply {
	return new Reply(status, body, { "content-type": "text/html; charset=utf-8", ...headers });
}

//ハンドラの戻り値を Reply にそろえる
export function toReply(result: any): Reply {
	if (result instanceof Reply) return result;
	if (result === undefined || result === null) return new Reply(204);
	if (typeof result == "string") return text(result);
	if (Buffer.isBuffer(result)) return new Reply(200, result, { "content-type": "application/octet-stream" });
	return json(result);
}

export function sendReply(res: ServerResponse, reply: Reply) {
	if (res.headersSent) {
		res.end();
		return;
	}
	res.writeHead(reply.status, reply.headers);
	res.end(reply.status == 204 || reply.status == 304 ? undefined : reply.body);
}
