import { fetchJson } from "../net/fetchJson.js"

const BASE = "https://api.anthropic.com/v1";
const VERSION = "2023-06-01";

//@anthropic-ai/sdk の置き換え。使っているエンドポイントだけ
export class AnthropicClient {
	constructor(private apiKey: string) {}

	private headers() {
		return { "x-api-key": this.apiKey, "anthropic-version": VERSION };
	}

	//GET /models → { data: [...] }
	listModels(): Promise<any> {
		return fetchJson(`${BASE}/models?limit=1000`, { headers: this.headers() });
	}

	//POST /messages
	createMessage(body: { model: string, max_tokens: number, messages: any[] }): Promise<any> {
		if (typeof body.model != "string" || body.model.length > 128) throw new Error("invalid model");
		return fetchJson(`${BASE}/messages`, { headers: this.headers(), body });
	}
}
