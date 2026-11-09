import { HttpError } from "../net/errors.js"
import { fetchJson } from "../net/fetchJson.js"
import { request, Transport } from "../net/request.js"
import { parseSse } from "../net/sse.js"

const BASE = "https://api.anthropic.com/v1";
const VERSION = "2023-06-01";
const STREAM_TIMEOUT_MS = 10 * 60 * 1000;	//stream 全体の期限
const STREAM_IDLE_MS = 2 * 60 * 1000;		//stream の無通信の期限 (応答中は ping が届く)
const STREAM_MAX_BYTES = 32 * 1024 * 1024;

export interface AiClientOptions {
	transport?: Transport;	//差し替え (テスト用)。既定は request
}

type MessageBody = { model: string, max_tokens: number, messages: any[] };

//@anthropic-ai/sdk の置き換え。使っているエンドポイントだけ
export class AnthropicClient {
	constructor(private apiKey: string, private options: AiClientOptions = {}) {}

	private headers() {
		return { "x-api-key": this.apiKey, "anthropic-version": VERSION };
	}

	//GET /models → { data: [...] }
	listModels(): Promise<any> {
		return fetchJson(`${BASE}/models?limit=1000`, { headers: this.headers(), transport: this.options.transport });
	}

	//POST /messages
	createMessage(body: MessageBody): Promise<any> {
		validateModel(body.model);
		return fetchJson(`${BASE}/messages`, { headers: this.headers(), body, transport: this.options.transport });
	}

	//POST /messages (stream: true)。SSE の各イベントを JSON で返す。event: error は例外にする
	async *createMessageStream(body: MessageBody, opts: { signal?: AbortSignal } = {}): AsyncGenerator<any, void, unknown> {
		validateModel(body.model);
		const res = await (this.options.transport ?? request)(`${BASE}/messages`, {
			method: "POST",
			headers: { ...this.headers(), "Accept": "text/event-stream", "Content-Type": "application/json" },
			body: JSON.stringify({ ...body, stream: true }),
			timeoutMs: STREAM_TIMEOUT_MS,
			idleTimeoutMs: STREAM_IDLE_MS,
			maxBodyBytes: STREAM_MAX_BYTES,
			signal: opts.signal,
		});
		if (res.status < 200 || res.status >= 300) {
			throw new HttpError(`api.anthropic.com responded ${res.status}`, res.status, await res.text(500));
		}
		for await (const ev of parseSse(res.body())) {
			const data: unknown = JSON.parse(ev.data);
			if (!data || typeof data != "object" || Array.isArray(data)) throw new Error("anthropic stream event is not an object");
			const event = data as any;
			if (ev.event == "error" || event.type == "error") {
				const type = typeof event.error?.type == "string" ? event.error.type.substring(0, 100) : "unknown";
				throw new Error(`anthropic stream error: ${type}`);
			}
			yield event;
		}
	}
}

function validateModel(model: unknown) {
	if (typeof model != "string" || model.length > 128) throw new Error("invalid model");
}
