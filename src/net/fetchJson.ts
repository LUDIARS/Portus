import { HttpError } from "./errors.js"
import { request, Transport } from "./request.js"

//外部 API 呼び出し用の JSON 要求。通信は request (自前の HTTP/1.1、圧縮・リダイレクトなし、本文の上限つき)
export interface JsonRequest {
	method?: "GET" | "POST";
	headers?: Record<string, string>;
	body?: any;
	timeoutMs?: number;
	maxBodyBytes?: number;	//応答本文の上限 (既定 8 MiB)
	signal?: AbortSignal;
	transport?: Transport;	//差し替え (テスト用)。既定は request
}

const MAX_ERROR_BODY = 500;

export async function fetchJson(url: string, req: JsonRequest = {}): Promise<any> {
	const headers: Record<string, string> = { "Accept": "application/json", ...(req.headers || {}) };
	let body: string | undefined;
	if (req.body !== undefined) {
		headers["Content-Type"] = "application/json";
		body = JSON.stringify(req.body);
	}

	const res = await (req.transport ?? request)(url, {
		method: req.method || (body !== undefined ? "POST" : "GET"),
		headers,
		body,
		timeoutMs: req.timeoutMs ?? 120000,
		maxBodyBytes: req.maxBodyBytes,
		signal: req.signal,
	});

	if (res.status < 200 || res.status >= 300) {
		//エラー本文は先頭だけ読んで閉じる (全部は読まない)
		const text = await res.text(MAX_ERROR_BODY);
		throw new HttpError(`${new URL(url).host} responded ${res.status}`, res.status, text);
	}
	const text = await res.text();
	return text == "" ? null : JSON.parse(text);
}

//URL パスに埋め込む識別子(モデル名など)を検証する
export function pathSegment(value: string, name: string): string {
	if (typeof value != "string" || !/^[A-Za-z0-9._:-]{1,128}$/.test(value)) {
		throw new Error(`invalid ${name}`);
	}
	return encodeURIComponent(value);
}
