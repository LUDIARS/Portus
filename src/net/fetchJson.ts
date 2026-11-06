//外部 API 呼び出し用の JSON fetch。タイムアウト・リダイレクト禁止・エラー本文の切り詰めを揃える
export class HttpError extends Error {
	constructor(message: string, public status: number, public body: string) {
		super(message);
	}
}

export interface JsonRequest {
	method?: "GET" | "POST";
	headers?: Record<string, string>;
	body?: any;
	timeoutMs?: number;
}

export async function fetchJson(url: string, req: JsonRequest = {}): Promise<any> {
	const headers: Record<string, string> = { "Accept": "application/json", ...(req.headers || {}) };
	let body: string | undefined;
	if (req.body !== undefined) {
		headers["Content-Type"] = "application/json";
		body = JSON.stringify(req.body);
	}

	const res = await fetch(url, {
		method: req.method || (body ? "POST" : "GET"),
		headers,
		body,
		redirect: "error",
		signal: AbortSignal.timeout(req.timeoutMs ?? 120000),
	});

	const text = await res.text();
	if (!res.ok) {
		throw new HttpError(`${new URL(url).host} responded ${res.status}`, res.status, text.substring(0, 500));
	}
	return text == "" ? null : JSON.parse(text);
}

//URL パスに埋め込む識別子(モデル名など)を検証する
export function pathSegment(value: string, name: string): string {
	if (typeof value != "string" || !/^[A-Za-z0-9._:-]{1,128}$/.test(value)) {
		throw new Error(`invalid ${name}`);
	}
	return encodeURIComponent(value);
}
