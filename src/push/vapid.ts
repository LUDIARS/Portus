import { createPrivateKey, KeyObject, sign } from "node:crypto"

//VAPID (RFC 8292) の Authorization ヘッダを作る。署名は ES256 (node:crypto)
export interface VapidKeys {
	subject: string;		//mailto: か https: の連絡先
	publicKey: string;		//base64url の非圧縮公開鍵 (65 byte)
	privateKey: string;		//base64url の秘密鍵 (32 byte)
}

function toPrivateKey(keys: VapidKeys): KeyObject {
	const pub = Buffer.from(keys.publicKey, "base64url");
	const d = Buffer.from(keys.privateKey, "base64url");
	if (pub.length != 65 || pub[0] != 0x04) throw new Error("VAPID public key must be 65-byte uncompressed P-256");
	if (d.length != 32) throw new Error("VAPID private key must be 32 bytes");

	return createPrivateKey({
		key: {
			kty: "EC",
			crv: "P-256",
			d: d.toString("base64url"),
			x: pub.subarray(1, 33).toString("base64url"),
			y: pub.subarray(33, 65).toString("base64url"),
		},
		format: "jwk",
	});
}

function encodeJson(value: any): string {
	return Buffer.from(JSON.stringify(value)).toString("base64url");
}

//audience は push サービスの origin (例: https://fcm.googleapis.com)
export function createVapidAuthorization(keys: VapidKeys, audience: string, ttlSec: number = 12 * 60 * 60, now: number = Date.now()): string {
	if (!/^(mailto:|https:)/.test(keys.subject)) throw new Error("VAPID subject must be mailto: or https:");

	const header = encodeJson({ typ: "JWT", alg: "ES256" });
	const claims = encodeJson({ aud: audience, exp: Math.floor(now / 1000) + ttlSec, sub: keys.subject });
	const input = `${header}.${claims}`;
	const signature = sign("sha256", Buffer.from(input), { key: toPrivateKey(keys), dsaEncoding: "ieee-p1363" });

	return `vapid t=${input}.${signature.toString("base64url")}, k=${keys.publicKey}`;
}
