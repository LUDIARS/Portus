import { createCipheriv, createECDH, hkdfSync, randomBytes } from "node:crypto"

//Web Push のメッセージ暗号化 (RFC 8291 + RFC 8188 aes128gcm, 単一レコード)
//鍵交換 P-256 ECDH / 鍵導出 HKDF-SHA256 / 暗号 AES-128-GCM。プリミティブは node:crypto を使う
const RECORD_SIZE = 4096;
const TAG_LENGTH = 16;

export interface SubscriptionKeys {
	p256dh: string;	//base64url, ブラウザの公開鍵 (65 byte)
	auth: string;	//base64url, 認証シークレット (16 byte)
}

function hkdf(salt: Buffer, ikm: Buffer, info: Buffer, length: number): Buffer {
	return Buffer.from(hkdfSync("sha256", ikm, salt, info, length));
}

function label(text: string): Buffer {
	return Buffer.from(text + "\0", "ascii");
}

//テストで固定値を差し込めるようにしたもの
export interface EncryptOptions {
	salt?: Buffer;
	serverPrivateKey?: Buffer;
}

export function encryptPayload(keys: SubscriptionKeys, payload: Buffer, options: EncryptOptions = {}): Buffer {
	const uaPublic = Buffer.from(keys.p256dh, "base64url");
	const authSecret = Buffer.from(keys.auth, "base64url");
	if (uaPublic.length != 65 || uaPublic[0] != 0x04) throw new Error("p256dh must be 65-byte uncompressed P-256");
	if (authSecret.length != 16) throw new Error("auth must be 16 bytes");
	if (payload.length + 1 + TAG_LENGTH > RECORD_SIZE) throw new Error("payload too large for a single record");

	const ecdh = createECDH("prime256v1");
	if (options.serverPrivateKey) ecdh.setPrivateKey(options.serverPrivateKey);
	else ecdh.generateKeys();
	const asPublic = ecdh.getPublicKey();
	const sharedSecret = ecdh.computeSecret(uaPublic);

	const keyInfo = Buffer.concat([label("WebPush: info"), uaPublic, asPublic]);
	const ikm = hkdf(authSecret, sharedSecret, keyInfo, 32);

	const salt = options.salt ?? randomBytes(16);
	const cek = hkdf(salt, ikm, label("Content-Encoding: aes128gcm"), 16);
	const nonce = hkdf(salt, ikm, label("Content-Encoding: nonce"), 12);

	//最終レコードの区切り 0x02 を付けて暗号化する
	const cipher = createCipheriv("aes-128-gcm", cek, nonce);
	const encrypted = Buffer.concat([cipher.update(Buffer.concat([payload, Buffer.from([0x02])])), cipher.final(), cipher.getAuthTag()]);

	const header = Buffer.alloc(16 + 4 + 1);
	salt.copy(header, 0);
	header.writeUInt32BE(RECORD_SIZE, 16);
	header[20] = asPublic.length;

	return Buffer.concat([header, asPublic, encrypted]);
}
