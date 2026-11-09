import { test } from "node:test"
import * as assert from "node:assert"
import { execFileSync } from "node:child_process"
import * as fs from "node:fs"
import * as os from "node:os"
import * as path from "node:path"
import * as tls from "node:tls"
import { NetError } from "../src/net/errors.js"
import { request } from "../src/net/request.js"
import { TlsSessionCache } from "../src/net/tlsSessions.js"

//テスト用の自己署名証明書は実行時に openssl で作る (秘密鍵をリポに置かない)。openssl が無ければ飛ばす
function makeCert(dir: string, name: string, san: string): { key: string, cert: string } | null {
	const keyPath = path.join(dir, `${name}.key`);
	const certPath = path.join(dir, `${name}.crt`);
	try {
		execFileSync("openssl", [
			"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
			"-keyout", keyPath, "-out", certPath, "-days", "1", "-subj", `/CN=${name}`, "-addext", `subjectAltName=${san}`,
		], { stdio: "ignore" });
	} catch (ex) {
		return null;
	}
	return { key: fs.readFileSync(keyPath, "utf8"), cert: fs.readFileSync(certPath, "utf8") };
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "portus-tls-"));
const localhost = makeCert(dir, "localhost", "DNS:localhost,IP:127.0.0.1");
const other = makeCert(dir, "other.test", "DNS:other.test");
const skip = !localhost || !other ? "openssl が無い" : false;

async function tlsServer(cert: { key: string, cert: string }, options: tls.TlsOptions = {}) {
	const server = tls.createServer({ key: cert.key, cert: cert.cert, ALPNProtocols: ["http/1.1"], ...options }, (socket) => {
		socket.on("error", () => {});
		socket.once("data", () => socket.end("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
	});
	server.on("tlsClientError", () => {});
	await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
	return { port: (server.address() as any).port as number, close: () => new Promise<void>((resolve) => server.close(() => resolve())) };
}

async function tlsError(promise: Promise<unknown>) {
	await assert.rejects(promise, (ex: unknown) => ex instanceof NetError && ex.code == "TLS");
}

test("TLS: 既定は TLS 1.3 以上。証明書と名前を検証する", { skip }, async () => {
	const srv = await tlsServer(localhost!);
	try {
		const res = await request(`https://localhost:${srv.port}/`, { ca: localhost!.cert, allowInternalAddresses: true });
		assert.strictEqual(await res.text(), "ok");
		//IP で接続しても、証明書の IP SAN で照合する
		const byIp = await request(`https://127.0.0.1:${srv.port}/`, { ca: localhost!.cert, allowInternalAddresses: true });
		assert.strictEqual(await byIp.text(), "ok");
		//信頼していない CA
		await tlsError(request(`https://localhost:${srv.port}/`, { allowInternalAddresses: true }));
	} finally {
		await srv.close();
	}
});

test("TLS: 名前が合わない証明書は拒否する", { skip }, async () => {
	const srv = await tlsServer(other!);
	try {
		await tlsError(request(`https://localhost:${srv.port}/`, { ca: other!.cert, allowInternalAddresses: true }));
	} finally {
		await srv.close();
	}
});

test("TLS: 相手が TLS 1.2 までなら、既定では繋がず、明示すれば繋ぐ", { skip }, async () => {
	const srv = await tlsServer(localhost!, { maxVersion: "TLSv1.2" });
	try {
		await tlsError(request(`https://localhost:${srv.port}/`, { ca: localhost!.cert, allowInternalAddresses: true }));
		const res = await request(`https://localhost:${srv.port}/`, { ca: localhost!.cert, allowInternalAddresses: true, minTlsVersion: "TLSv1.2" });
		assert.strictEqual(await res.text(), "ok");
	} finally {
		await srv.close();
	}
});

test("TLS: 接続は使い回さず、セッションを再開して張り直す", { skip }, async () => {
	const srv = await tlsServer(localhost!);
	const sessions = new TlsSessionCache();
	try {
		const options = { ca: localhost!.cert, allowInternalAddresses: true, tlsSessions: sessions };
		const first = await request(`https://localhost:${srv.port}/`, options);
		assert.strictEqual(first.tlsSessionReused, false);
		await first.text();
		assert.strictEqual(sessions.size, 1);
		const second = await request(`https://localhost:${srv.port}/`, options);
		assert.strictEqual(second.tlsSessionReused, true);
		await second.text();
		//信頼する CA が違えばセッションを共有しない
		const otherTrust = await request(`https://localhost:${srv.port}/`, { ...options, ca: [localhost!.cert, other!.cert] });
		assert.strictEqual(otherTrust.tlsSessionReused, false);
		await otherTrust.text();
	} finally {
		await srv.close();
	}
});
