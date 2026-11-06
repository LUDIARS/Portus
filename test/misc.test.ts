import { test } from "node:test"
import * as assert from "node:assert"
import { parseEnv } from "../src/env.js"
import { encodeCommand, RespParser, RedisReplyError } from "../src/redis/resp.js"
import { signRequest } from "../src/aws/sigv4.js"
import { parseCredentialsFile } from "../src/aws/credentials.js"
import { redact } from "../src/log/record.js"
import { uuidv4 } from "../src/id.js"

test(".env の解析", () => {
	const env = parseEnv([
		"# comment",
		"A=1",
		"export B = two ",
		'C="x # not comment\\ny"',
		"D='raw\\n'",
		"E=value # comment",
		"1BAD=x",
		"__proto__=x",
	].join("\n"));
	assert.strictEqual(env.A, "1");
	assert.strictEqual(env.B, "two");
	assert.strictEqual(env.C, "x # not comment\ny");
	assert.strictEqual(env.D, "raw\\n");
	assert.strictEqual(env.E, "value");
	assert.strictEqual(env["1BAD"], undefined);
	assert.strictEqual(Object.getPrototypeOf(env), null);
});

test("RESP のエンコードと分割受信の解析", () => {
	assert.strictEqual(encodeCommand(["SET", "k", "あ"]).toString(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nあ\r\n");

	const p = new RespParser();
	const bytes = Buffer.from("+OK\r\n$3\r\nあ\r\n*2\r\n$1\r\na\r\n$-1\r\n:5\r\n-ERR bad\r\n");
	const replies = [...p.push(bytes.subarray(0, 9)), ...p.push(bytes.subarray(9))];
	assert.strictEqual(replies[0], "OK");
	assert.strictEqual(replies[1], "あ");
	assert.deepStrictEqual(replies[2], ["a", null]);
	assert.strictEqual(replies[3], 5);
	assert.ok(replies[4] instanceof RedisReplyError);
});

test("RESP の異常な長さは拒否する", () => {
	assert.throws(() => new RespParser().push(Buffer.from("$999999999999\r\n")));
	assert.throws(() => new RespParser().push(Buffer.from("?x\r\n")));
});

//署名値は秘密鍵とスコープだけで決まり、アクセスキー ID は Credential= に載るだけで計算に入らない。
//そのため ID はキー形式でないダミーにし、署名値は AWS ドキュメントの例 (S3 GET Object) と照合する
test("SigV4 は AWS ドキュメントの S3 GET Object 例と一致する", () => {
	const headers = signRequest({
		method: "GET",
		host: "examplebucket.s3.amazonaws.com",
		path: "/test.txt",
		region: "us-east-1",
		service: "s3",
		payload: Buffer.alloc(0),
		headers: { range: "bytes=0-9" },
		date: new Date("2013-05-24T00:00:00Z"),
	}, { accessKeyId: "TEST-ACCESS-KEY-ID", secretAccessKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY" });

	assert.strictEqual(headers["authorization"],
		"AWS4-HMAC-SHA256 Credential=TEST-ACCESS-KEY-ID/20130524/us-east-1/s3/aws4_request, " +
		"SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, " +
		"Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
});

test("共有認証ファイルから指定プロファイルを読む", () => {
	const text = "[default]\naws_access_key_id = A\naws_secret_access_key = B\n\n[other]\naws_access_key_id=C\naws_secret_access_key=D\naws_session_token=E\n";
	assert.deepStrictEqual(parseCredentialsFile(text, "other"), { accessKeyId: "C", secretAccessKey: "D", sessionToken: "E" });
	assert.strictEqual(parseCredentialsFile(text, "none"), null);
});

test("ログは秘匿キーをマスクし、循環参照で落ちない", () => {
	const obj: any = { query: { session: "s", token: "t", id: 1 }, Authorization: "Bearer x" };
	obj.self = obj;
	const out = redact(obj);
	assert.strictEqual(out.query.session, "[REDACTED]");
	assert.strictEqual(out.query.token, "[REDACTED]");
	assert.strictEqual(out.query.id, 1);
	assert.strictEqual(out.Authorization, "[REDACTED]");
	assert.strictEqual(out.self, "[Circular]");
});

test("uuidv4 の形式", () => {
	assert.match(uuidv4(), /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
});
