import { createHash, randomBytes } from "node:crypto"
import { IncomingHttpHeaders } from "node:http"

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

//Sec-WebSocket-Accept の値
export function acceptKey(key: string): string {
	return createHash("sha1").update(key + GUID).digest("base64");
}

//クライアント用の Sec-WebSocket-Key
export function generateKey(): string {
	return randomBytes(16).toString("base64");
}

function headerHasToken(value: string | string[] | undefined, token: string): boolean {
	const text = Array.isArray(value) ? value.join(",") : (value || "");
	return text.split(",").some((t) => t.trim().toLowerCase() == token);
}

//サーバ側: アップグレード要求を検証し、問題があれば理由を返す
export function validateUpgrade(method: string | undefined, headers: IncomingHttpHeaders): string | null {
	if (method != "GET") return "method must be GET";
	if (!headerHasToken(headers["upgrade"], "websocket")) return "missing upgrade: websocket";
	if (!headerHasToken(headers["connection"], "upgrade")) return "missing connection: upgrade";
	if (headers["sec-websocket-version"] != "13") return "unsupported version";

	const key = headers["sec-websocket-key"];
	if (typeof key != "string" || !/^[A-Za-z0-9+/]{22}==$/.test(key)) return "invalid key";
	return null;
}

//サーバ側: 101 応答ヘッダ
export function buildAcceptResponse(key: string): string {
	return [
		"HTTP/1.1 101 Switching Protocols",
		"Upgrade: websocket",
		"Connection: Upgrade",
		`Sec-WebSocket-Accept: ${acceptKey(key)}`,
		"", "",
	].join("\r\n");
}
