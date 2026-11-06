import { createHash, createHmac } from "node:crypto"

//AWS Signature Version 4 (ヘッダ署名) の最小実装
export interface AwsCredentials {
	accessKeyId: string;
	secretAccessKey: string;
	sessionToken?: string;
	expiration?: Date;
}

export interface SignInput {
	method: string;
	host: string;
	path: string;				//エンコード済みのパス
	region: string;
	service: string;
	payload: Buffer;
	headers?: Record<string, string>;
	date?: Date;
}

export function sha256Hex(data: Buffer | string): string {
	return createHash("sha256").update(data).digest("hex");
}

function hmac(key: Buffer | string, data: string): Buffer {
	return createHmac("sha256", key).update(data).digest();
}

//RFC 3986 の非予約文字以外をエンコードする(S3 のキー用。"/" は区切りとして残す)
export function encodeKeyPath(key: string): string {
	return key.split("/").map((seg) => encodeURIComponent(seg).replace(/[!'()*]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase())).join("/");
}

function amzDate(date: Date): string {
	return date.toISOString().replace(/[-:]/g, "").replace(/\.\d{3}/, "");
}

//署名済みヘッダ(Host を除く)を返す
export function signRequest(input: SignInput, creds: AwsCredentials): Record<string, string> {
	const date = amzDate(input.date ?? new Date());
	const day = date.substring(0, 8);
	const payloadHash = sha256Hex(input.payload);

	const headers: Record<string, string> = {
		...(input.headers || {}),
		"host": input.host,
		"x-amz-content-sha256": payloadHash,
		"x-amz-date": date,
	};
	if (creds.sessionToken) headers["x-amz-security-token"] = creds.sessionToken;

	const names = Object.keys(headers).map((h) => h.toLowerCase()).sort();
	const lower: Record<string, string> = {};
	for (const k of Object.keys(headers)) lower[k.toLowerCase()] = String(headers[k]).trim().replace(/\s+/g, " ");

	const canonicalHeaders = names.map((h) => `${h}:${lower[h]}\n`).join("");
	const signedHeaders = names.join(";");
	const canonicalRequest = [input.method, input.path, "", canonicalHeaders, signedHeaders, payloadHash].join("\n");

	const scope = `${day}/${input.region}/${input.service}/aws4_request`;
	const stringToSign = ["AWS4-HMAC-SHA256", date, scope, sha256Hex(canonicalRequest)].join("\n");

	const kDate = hmac("AWS4" + creds.secretAccessKey, day);
	const kRegion = hmac(kDate, input.region);
	const kService = hmac(kRegion, input.service);
	const kSigning = hmac(kService, "aws4_request");
	const signature = createHmac("sha256", kSigning).update(stringToSign).digest("hex");

	const out: Record<string, string> = {};
	for (const k of names) if (k != "host") out[k] = lower[k]!;
	out["authorization"] = `AWS4-HMAC-SHA256 Credential=${creds.accessKeyId}/${scope}, SignedHeaders=${signedHeaders}, Signature=${signature}`;
	return out;
}
