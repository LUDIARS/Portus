import { test } from "node:test"
import * as assert from "node:assert"
import { createDecipheriv, createECDH, createPublicKey, hkdfSync, verify } from "node:crypto"
import { encryptPayload } from "../src/push/encrypt.js"
import { createVapidAuthorization } from "../src/push/vapid.js"
import { isAllowedEndpoint } from "../src/push/endpoint.js"

//RFC 8291 Appendix A の検証値
const RFC = {
	plaintext: "When I grow up, I want to be a watermelon",
	asPrivate: "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw",
	uaPrivate: "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94",
	uaPublic: "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4",
	authSecret: "BTBZMqHH6r4Tts7J_aSIgg",
	salt: "DGv6ra1nlYgDCS1FRnbzlw",
	body: "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN",
};

test("aes128gcm 暗号化が RFC 8291 の検証値と一致する", () => {
	const body = encryptPayload(
		{ p256dh: RFC.uaPublic, auth: RFC.authSecret },
		Buffer.from(RFC.plaintext),
		{ salt: Buffer.from(RFC.salt, "base64url"), serverPrivateKey: Buffer.from(RFC.asPrivate, "base64url") },
	);
	assert.strictEqual(body.toString("base64url"), RFC.body);
});

//受信側 (ブラウザ) の手順で復号できること
function decrypt(body: Buffer, uaPrivate: Buffer, authSecret: Buffer): Buffer {
	const salt = body.subarray(0, 16);
	const idlen = body[20]!;
	const asPublic = body.subarray(21, 21 + idlen);
	const ct = body.subarray(21 + idlen);

	const ecdh = createECDH("prime256v1");
	ecdh.setPrivateKey(uaPrivate);
	const shared = ecdh.computeSecret(asPublic);
	const info = Buffer.concat([Buffer.from("WebPush: info\0"), ecdh.getPublicKey(), asPublic]);
	const ikm = Buffer.from(hkdfSync("sha256", shared, authSecret, info, 32));
	const cek = Buffer.from(hkdfSync("sha256", ikm, salt, Buffer.from("Content-Encoding: aes128gcm\0"), 16));
	const nonce = Buffer.from(hkdfSync("sha256", ikm, salt, Buffer.from("Content-Encoding: nonce\0"), 12));

	const d = createDecipheriv("aes-128-gcm", cek, nonce);
	d.setAuthTag(ct.subarray(ct.length - 16));
	const plain = Buffer.concat([d.update(ct.subarray(0, ct.length - 16)), d.final()]);
	assert.strictEqual(plain[plain.length - 1], 0x02);
	return plain.subarray(0, plain.length - 1);
}

test("ランダムな鍵で暗号化しても受信側で復号できる", () => {
	const body = encryptPayload({ p256dh: RFC.uaPublic, auth: RFC.authSecret }, Buffer.from("{\"title\":\"新しい通知\"}"));
	const plain = decrypt(body, Buffer.from(RFC.uaPrivate, "base64url"), Buffer.from(RFC.authSecret, "base64url"));
	assert.strictEqual(plain.toString(), "{\"title\":\"新しい通知\"}");
});

test("大きすぎる本文と不正な鍵は拒否する", () => {
	assert.throws(() => encryptPayload({ p256dh: RFC.uaPublic, auth: RFC.authSecret }, Buffer.alloc(5000)));
	assert.throws(() => encryptPayload({ p256dh: "AAAA", auth: RFC.authSecret }, Buffer.from("x")));
});

test("VAPID の JWT は公開鍵で検証できる", () => {
	const ecdh = createECDH("prime256v1");
	ecdh.generateKeys();
	const keys = {
		subject: "mailto:info@test.com",
		publicKey: ecdh.getPublicKey().toString("base64url"),
		privateKey: ecdh.getPrivateKey().toString("base64url"),
	};
	const header = createVapidAuthorization(keys, "https://fcm.googleapis.com", 3600, 1700000000000);
	const m = /^vapid t=([^.]+)\.([^.]+)\.([^,]+), k=(.+)$/.exec(header);
	assert.ok(m);

	const claims = JSON.parse(Buffer.from(m![2]!, "base64url").toString());
	assert.deepStrictEqual(claims, { aud: "https://fcm.googleapis.com", exp: 1700003600, sub: "mailto:info@test.com" });
	assert.strictEqual(m![4], keys.publicKey);

	const pub = ecdh.getPublicKey();
	const key = createPublicKey({ key: { kty: "EC", crv: "P-256", x: pub.subarray(1, 33).toString("base64url"), y: pub.subarray(33).toString("base64url") }, format: "jwk" });
	const ok = verify("sha256", Buffer.from(`${m![1]}.${m![2]}`), { key, dsaEncoding: "ieee-p1363" }, Buffer.from(m![3]!, "base64url"));
	assert.ok(ok);
});

test("push エンドポイントは https の既知サービスだけ許可する", () => {
	assert.ok(isAllowedEndpoint("https://fcm.googleapis.com/fcm/send/abc"));
	assert.ok(isAllowedEndpoint("https://wns2-par02p.notify.windows.com/w/?token=x"));
	assert.ok(!isAllowedEndpoint("http://fcm.googleapis.com/fcm/send/abc"));
	assert.ok(!isAllowedEndpoint("https://169.254.169.254/latest/meta-data/"));
	assert.ok(!isAllowedEndpoint("https://evil.example/fcm.googleapis.com"));
	assert.ok(!isAllowedEndpoint("https://user:pw@fcm.googleapis.com/"));
	assert.ok(!isAllowedEndpoint("not a url"));
});
