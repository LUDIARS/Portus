import { fetchJson, pathSegment } from "../net/fetchJson.js"

const BASE = "https://generativelanguage.googleapis.com/v1beta";

//@google/generative-ai の置き換え。キーは URL ではなくヘッダで渡す(ログに残さないため)
export class GeminiClient {
	constructor(private apiKey: string) {}

	private headers() {
		return { "x-goog-api-key": this.apiKey };
	}

	//GET /models → { models: [...] }
	listModels(): Promise<any> {
		return fetchJson(`${BASE}/models?pageSize=1000`, { headers: this.headers() });
	}

	//POST /models/{model}:generateContent の応答テキスト(SDK の response.text() 相当)
	async generateText(model: string, body: { contents: any[], generationConfig?: any }): Promise<string> {
		const name = pathSegment(model.replace(/^models\//, ""), "model");
		const res = await fetchJson(`${BASE}/models/${name}:generateContent`, { headers: this.headers(), body });
		const parts = res?.candidates?.[0]?.content?.parts || [];
		return parts.map((p: any) => p.text || "").join("");
	}
}
