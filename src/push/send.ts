import { request } from "../net/request.js"
import { encryptPayload, SubscriptionKeys } from "./encrypt.js"
import { DEFAULT_PUSH_HOSTS, isAllowedEndpoint } from "./endpoint.js"
import { createVapidAuthorization, VapidKeys } from "./vapid.js"

export interface PushSubscription {
	endpoint: string;
	keys: SubscriptionKeys;
}

export class PushError extends Error {
	constructor(message: string, public statusCode: number) {
		super(message);
	}
}

export interface SendOptions {
	ttlSec?: number;					//push サービスでの保持秒数 (既定 1 日)
	allowedHosts?: readonly string[];	//送信を許す push サービスのホスト
	timeoutMs?: number;
}

//web-push の sendNotification 相当。暗号化した本文を購読エンドポイントへ POST する
export async function sendNotification(sub: PushSubscription, payload: string, vapid: VapidKeys, options: SendOptions = {}): Promise<number> {
	if (!isAllowedEndpoint(sub.endpoint, options.allowedHosts ?? DEFAULT_PUSH_HOSTS)) throw new PushError("endpoint is not an allowed push service", 0);
	const ttlSec = options.ttlSec ?? 24 * 60 * 60;

	const body = encryptPayload(sub.keys, Buffer.from(payload, "utf8"));
	const res = await request(sub.endpoint, {
		method: "POST",
		timeoutMs: options.timeoutMs ?? 10000,
		minTlsVersion: "TLSv1.2",	//push サービスによっては TLS 1.2 まで
		headers: {
			"Authorization": createVapidAuthorization(vapid, new URL(sub.endpoint).origin),
			"Content-Encoding": "aes128gcm",
			"Content-Type": "application/octet-stream",
			"TTL": String(ttlSec),
		},
		body,
	});

	if (res.status < 200 || res.status >= 300) {
		const text = await res.text(200);
		throw new PushError(`push service responded ${res.status}: ${text}`, res.status);
	}
	res.close();
	return res.status;
}
