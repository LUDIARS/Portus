import { fetchJson } from "../net/fetchJson.js"

const BASE = "https://api.openai.com/v1";

//openai SDK の置き換え。使っているエンドポイントだけ
export class OpenAIClient {
	constructor(private apiKey: string) {}

	private headers() {
		return { "Authorization": `Bearer ${this.apiKey}` };
	}

	//GET /models → { data: [...] }
	listModels(): Promise<any> {
		return fetchJson(`${BASE}/models`, { headers: this.headers() });
	}

	//POST /chat/completions
	chatCompletions(body: { model: string, messages: any[], response_format?: any }): Promise<any> {
		if (typeof body.model != "string" || body.model.length > 128) throw new Error("invalid model");
		return fetchJson(`${BASE}/chat/completions`, { headers: this.headers(), body });
	}

	//POST /realtime/sessions (エフェメラルキー発行)
	createRealtimeSession(body: any): Promise<any> {
		return fetchJson(`${BASE}/realtime/sessions`, { headers: this.headers(), body });
	}
}
