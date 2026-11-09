import { test } from "node:test"
import * as assert from "node:assert"
import { AnthropicClient } from "../src/ai/anthropic.js"
import { OpenAIClient } from "../src/ai/openai.js"
import { HttpError } from "../src/net/errors.js"
import { HttpResponse, RequestOptions, Transport } from "../src/net/request.js"
import { parseSse, SseEvent } from "../src/net/sse.js"

const encoder = new TextEncoder();

async function* chunks(...parts: (string | Uint8Array)[]): AsyncGenerator<Uint8Array> {
	for (const p of parts) yield typeof p == "string" ? encoder.encode(p) : p;
}

async function collect<T>(it: AsyncIterable<T>): Promise<T[]> {
	const out: T[] = [];
	for await (const e of it) out.push(e);
	return out;
}

const delta = (text: string) => `event: content_block_delta\ndata: ${JSON.stringify({ type: "content_block_delta", index: 0, delta: { type: "text_delta", text } })}\n\n`;
const SAMPLE = [
	"event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}\n\n",
	": keep-alive\n\n",
	"event: ping\ndata: {\"type\":\"ping\"}\n\n",
	delta("こんにちは"),
	delta("、元気?"),
	"event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
].join("");

test("SSE: どのバイト位置で分割されても同じ (マルチバイト文字の途中、CRLF の間を含む)", async () => {
	const whole = await collect(parseSse(chunks(SAMPLE)));
	assert.deepStrictEqual(whole.map((e) => e.event), ["message_start", "ping", "content_block_delta", "content_block_delta", "message_stop"]);
	for (const text of [SAMPLE, SAMPLE.replace(/\n/g, "\r\n")]) {
		const bytes = encoder.encode(text);
		for (let i = 1; i < bytes.length; i++) {
			assert.deepStrictEqual(await collect(parseSse(chunks(bytes.slice(0, i), bytes.slice(i)))), whole, `split at ${i}`);
		}
	}
	assert.deepStrictEqual(await collect(parseSse(chunks(SAMPLE.replace(/\n/g, "\r")))), whole);
});

test("SSE: 複数行の data・id・末尾の空行なし", async () => {
	const events = await collect(parseSse(chunks("id: 7\ndata: a\ndata: b\n\nevent: x\n\ndata: last")));
	assert.deepStrictEqual(events, [{ event: "", data: "a\nb", id: "7" }, { event: "", data: "last" }] as SseEvent[]);
});

test("SSE: 区切りの来ない入力は上限ちょうどまで受け、+1 で打ち切る", async () => {
	assert.deepStrictEqual(await collect(parseSse(chunks("x".repeat(100)), 100)), []);
	await assert.rejects(collect(parseSse(chunks("x".repeat(101)), 100)), /size limit/);
	const piece = "data: " + "y".repeat(40);
	await assert.rejects(collect(parseSse(chunks(piece, piece, piece), 100)), /size limit/);
});

//送った要求を記録し、決めた応答を返す偽の通信
function fakeTransport(status: number, body: string): { transport: Transport, calls: { url: string, options: RequestOptions }[] } {
	const calls: { url: string, options: RequestOptions }[] = [];
	const transport: Transport = async (url, options = {}) => {
		calls.push({ url, options });
		const bytes = Buffer.from(body, "utf8");
		let closed = false;
		const res: HttpResponse = {
			status, reason: "", headers: Object.create(null), remoteAddress: "192.0.2.1", tlsSessionReused: false,
			async *body() { if (!closed) yield bytes; },
			async text(limit?: number) { return bytes.subarray(0, limit ?? bytes.length).toString("utf8"); },
			close() { closed = true; },
		};
		return res;
	};
	return { transport, calls };
}

test("Anthropic: createMessageStream は stream: true で送り、イベントを JSON で返す", async () => {
	const { transport, calls } = fakeTransport(200, SAMPLE);
	const client = new AnthropicClient("test-key", { transport });
	const events = await collect(client.createMessageStream({ model: "claude-test", max_tokens: 16, messages: [{ role: "user", content: "hi" }] }));
	const text = events.filter((e) => e.type == "content_block_delta").map((e) => e.delta.text).join("");
	assert.strictEqual(text, "こんにちは、元気?");
	const call = calls[0]!;
	assert.strictEqual(call.url, "https://api.anthropic.com/v1/messages");
	assert.strictEqual(call.options.method, "POST");
	assert.strictEqual((call.options.headers as Record<string, string>)["x-api-key"], "test-key");
	assert.strictEqual(JSON.parse(call.options.body as string).stream, true);
	assert.ok((call.options.timeoutMs ?? 0) > 0 && (call.options.idleTimeoutMs ?? 0) > 0 && (call.options.maxBodyBytes ?? 0) > 0);
});

test("Anthropic: stream の error イベント・2xx 以外・不正な data は例外", async () => {
	const error = "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}\n\n";
	const body = { model: "m", max_tokens: 1, messages: [] };
	await assert.rejects(collect(new AnthropicClient("k", fakeTransport(200, delta("a") + error)).createMessageStream(body)), /overloaded_error/);
	await assert.rejects(collect(new AnthropicClient("k", fakeTransport(529, "x".repeat(2000))).createMessageStream(body)), (ex: unknown) => ex instanceof HttpError && ex.status == 529 && ex.body.length == 500);
	await assert.rejects(collect(new AnthropicClient("k", fakeTransport(200, "data: [1]\n\n")).createMessageStream(body)), /not an object/);
	await assert.rejects(collect(new AnthropicClient("k", fakeTransport(200, "")).createMessageStream({ ...body, model: "x".repeat(129) })), /invalid model/);
});

test("Anthropic / OpenAI: createMessage と chatCompletions は差し替えた通信を使う", async () => {
	const anthropic = fakeTransport(200, JSON.stringify({ content: [{ type: "text", text: "ok" }] }));
	const msg = await new AnthropicClient("k", { transport: anthropic.transport }).createMessage({ model: "m", max_tokens: 1, messages: [] });
	assert.strictEqual(msg.content[0].text, "ok");
	assert.strictEqual(anthropic.calls[0]!.url, "https://api.anthropic.com/v1/messages");

	const openai = fakeTransport(200, JSON.stringify({ choices: [{ message: { content: "hi" } }] }));
	const res = await new OpenAIClient("sk", { transport: openai.transport }).chatCompletions({ model: "m", messages: [] });
	assert.strictEqual(res.choices[0].message.content, "hi");
	assert.strictEqual((openai.calls[0]!.options.headers as Record<string, string>)["Authorization"], "Bearer sk");
});
