import type { Socket } from "node:net"
import type { SecureVersion, TLSSocket } from "node:tls"
import { resolveAddress } from "./address.js"
import { openConnection } from "./connection.js"
import { NetError } from "./errors.js"
import { Method, ParseEvent, ResponseParser, serializeRequest } from "./http1.js"
import { ConcurrencyLimiter } from "./limiter.js"
import { defaultTlsSessions, TlsCa, TlsSessionCache } from "./tlsSessions.js"

//外部 HTTP 呼び出し。fetch (undici) と node:http を使わず、node:tls / node:net の上で HTTP/1.1 を組み立てる。
//- 要求ごとに独立した接続 (1 本の遅れ・失敗が他を止めない)。張り直しは TLS のセッション再開で軽くする
//- 既定は TLS 1.3 以上。平文 (http://) と内部向けアドレスは呼び出し側が明示したときだけ
//- 名前解決は 1 回。選んだ IP に接続する
//- 圧縮・リダイレクト・プロキシは扱わない。本文は読んだ分だけ受け取る (上限つき)
//- 期限は 3 段: 接続 (名前解決 + TCP + TLS)、無通信、全体 (本文を読み終えるまで)

export interface RequestOptions {
	method?: Method;
	headers?: Record<string, string>;
	body?: string | Buffer;
	timeoutMs?: number;					//全体の期限 (既定 120 秒)
	connectTimeoutMs?: number;			//接続の期限 (既定 10 秒)
	idleTimeoutMs?: number;				//無通信の期限 (既定は全体と同じ。応答を作るのに時間のかかる API のため)
	maxBodyBytes?: number;				//応答本文の上限 (既定 8 MiB)
	maxHeaderBytes?: number;			//状態行とヘッダの上限 (既定 16 KiB)
	signal?: AbortSignal;
	minTlsVersion?: SecureVersion;		//既定 TLSv1.3
	ca?: TlsCa;							//信頼する CA を差し替える (既定はシステムの CA)
	allowPlaintext?: boolean;			//http:// を許す
	allowInternalAddresses?: boolean;	//ループバック・プライベート・リンクローカルなどへの接続を許す
	limiter?: ConcurrencyLimiter;		//既定は接続先ごとに 16 本
	tlsSessions?: TlsSessionCache;
}

export interface HttpResponse {
	status: number;
	reason: string;
	headers: Record<string, string>;	//null プロトタイプ、名前は小文字
	remoteAddress: string;
	tlsSessionReused: boolean;
	//本文を読む。1 回だけ。読み終わるか途中でやめると接続を閉じる
	body(): AsyncGenerator<Buffer, void, unknown>;
	//本文を文字列で。limitBytes を渡すとそこで読むのをやめる (エラー本文の切り詰め用)
	text(limitBytes?: number): Promise<string>;
	//本文を読まずに閉じる
	close(): void;
}

export type Transport = (url: string, options?: RequestOptions) => Promise<HttpResponse>;

const DEFAULT_TIMEOUT_MS = 120000;
const DEFAULT_CONNECT_TIMEOUT_MS = 10000;
const DEFAULT_MAX_BODY_BYTES = 8 * 1024 * 1024;
const DEFAULT_MAX_HEADER_BYTES = 16 * 1024;
const MAX_HEADER_COUNT = 100;

export const defaultLimiter = new ConcurrencyLimiter(16);

export const request: Transport = async (url, options = {}) => {
	const target = new URL(url);
	const secure = target.protocol == "https:";
	if (!secure && !(target.protocol == "http:" && options.allowPlaintext)) throw new NetError("PROTOCOL", `${target.protocol} is not allowed`);
	if (target.username || target.password) throw new NetError("PROTOCOL", "credentials in the URL are not allowed");
	const host = target.hostname.replace(/^\[|\]$/g, "");
	const port = target.port ? Number(target.port) : secure ? 443 : 80;
	const body = options.body === undefined ? null : typeof options.body == "string" ? Buffer.from(options.body, "utf8") : options.body;
	const head = serializeRequest(options.method ?? (body ? "POST" : "GET"), target, options.headers ?? {}, body);

	//全体の期限と呼び出し側の中断を 1 つの signal にまとめる
	const lifetime = new AbortController();
	const totalTimer = setTimeout(() => lifetime.abort(new NetError("TIMEOUT", `${host} did not complete within ${options.timeoutMs ?? DEFAULT_TIMEOUT_MS}ms`)), options.timeoutMs ?? DEFAULT_TIMEOUT_MS);
	const onCallerAbort = () => lifetime.abort(new NetError("ABORTED", "request aborted"));
	if (options.signal?.aborted) onCallerAbort();
	else options.signal?.addEventListener("abort", onCallerAbort, { once: true });

	let socket: Socket | null = null;
	let release: (() => void) | null = null;
	let closed = false;
	const close = (reason?: unknown) => {
		if (closed) return;
		closed = true;
		clearTimeout(totalTimer);
		options.signal?.removeEventListener("abort", onCallerAbort);
		socket?.destroy(reason instanceof Error ? reason : undefined);
		release?.();
	};
	lifetime.signal.addEventListener("abort", () => close(lifetime.signal.reason), { once: true });

	try {
		const limiter = options.limiter ?? defaultLimiter;
		release = await limiter.acquire(`${host}:${port}`, lifetime.signal);
		if (closed) throw lifetime.signal.reason;

		//接続までの期限
		const connecting = new AbortController();
		const connectTimer = setTimeout(() => connecting.abort(new NetError("TIMEOUT", `cannot connect to ${host} within ${options.connectTimeoutMs ?? DEFAULT_CONNECT_TIMEOUT_MS}ms`)), options.connectTimeoutMs ?? DEFAULT_CONNECT_TIMEOUT_MS);
		const connectSignal = AbortSignal.any([lifetime.signal, connecting.signal]);
		let resolved;
		try {
			resolved = await withSignal(resolveAddress(host, options.allowInternalAddresses == true), connectSignal);
			socket = await openConnection({
				host, port, resolved, secure,
				minTlsVersion: options.minTlsVersion ?? "TLSv1.3",
				ca: options.ca,
				sessions: options.tlsSessions ?? defaultTlsSessions,
			}, connectSignal);
		} finally {
			clearTimeout(connectTimer);
		}
		if (closed) {
			socket.destroy();
			throw lifetime.signal.reason;
		}

		const idleMs = options.idleTimeoutMs ?? options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
		socket.setTimeout(idleMs, () => socket?.destroy(new NetError("TIMEOUT", `${host} sent nothing for ${idleMs}ms`)));
		socket.on("error", () => {});	//エラーは読み取り側 (for await) で受ける
		socket.write(head);
		if (body && body.length) socket.write(body);

		const parser = new ResponseParser({
			maxHeaderBytes: options.maxHeaderBytes ?? DEFAULT_MAX_HEADER_BYTES,
			maxHeaderCount: MAX_HEADER_COUNT,
			maxBodyBytes: options.maxBodyBytes ?? DEFAULT_MAX_BODY_BYTES,
		});
		const events = readEvents(socket, parser, host);
		const first = await events.next();
		if (first.done || first.value.type != "head") throw new NetError("CLOSED", `${host} closed the connection before responding`);

		const remoteAddress = socket.remoteAddress ?? resolved.address;
		const reused = secure ? (socket as TLSSocket).isSessionReused() : false;
		let consumed = false;
		async function* readBody(): AsyncGenerator<Buffer, void, unknown> {
			if (consumed) throw new Error("response body was already read");
			consumed = true;
			try {
				for (;;) {
					const next = await events.next();
					if (next.done || next.value.type == "end") return;
					if (next.value.type == "data") yield next.value.chunk;
				}
			} finally {
				close();
			}
		}
		return {
			status: first.value.head.status,
			reason: first.value.head.reason,
			headers: first.value.head.headers,
			remoteAddress,
			tlsSessionReused: reused,
			body: readBody,
			text: (limitBytes?: number) => readText(readBody(), limitBytes),
			close: () => close(),
		};
	} catch (ex) {
		const reason = closed && lifetime.signal.aborted ? lifetime.signal.reason : ex;
		close(reason);
		release?.();	//acquire を待つ間に閉じた場合、close の時点ではまだ枠を持っていない (解放関数は 1 回だけ効く)
		throw reason;
	}
};

async function* readEvents(socket: Socket, parser: ResponseParser, host: string): AsyncGenerator<ParseEvent, void, unknown> {
	try {
		for await (const chunk of socket) {
			for (const event of parser.push(chunk as Buffer)) {
				yield event;
				if (event.type == "end") return;
			}
		}
	} catch (ex) {
		if (ex instanceof NetError) throw ex;
		throw new NetError("CLOSED", `connection to ${host} failed (${(ex as any)?.code ?? (ex as Error)?.message})`);
	}
	parser.finish();
}

async function readText(body: AsyncGenerator<Buffer, void, unknown>, limitBytes?: number): Promise<string> {
	const parts: Buffer[] = [];
	let size = 0;
	for await (const chunk of body) {
		if (limitBytes !== undefined && size + chunk.length >= limitBytes) {
			parts.push(chunk.subarray(0, limitBytes - size));
			break;	//残りは読まない (generator の finally で接続を閉じる)
		}
		parts.push(chunk);
		size += chunk.length;
	}
	return new TextDecoder().decode(Buffer.concat(parts));
}

//signal が切れたら promise の完了を待たずに理由を投げる
function withSignal<T>(promise: Promise<T>, signal: AbortSignal): Promise<T> {
	signal.throwIfAborted();
	return new Promise((resolve, reject) => {
		const onAbort = () => reject(signal.reason);
		signal.addEventListener("abort", onAbort, { once: true });
		promise.then(
			(v) => { signal.removeEventListener("abort", onAbort); resolve(v); },
			(e) => { signal.removeEventListener("abort", onAbort); reject(e); },
		);
	});
}
