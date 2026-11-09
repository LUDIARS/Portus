import { createHash } from "node:crypto"

//TLS セッションチケットの再利用 (QUIC の 1-RTT 再開にあたる)。接続は使い回さず、張り直しのハンドシェイクだけを軽くする。
//0-RTT (early data) は再送攻撃を受けうるので使わない
const MAX_ENTRIES = 256;

export type TlsCa = string | Buffer | Array<string | Buffer>;

export class TlsSessionCache {
	private entries = new Map<string, Buffer>();

	get(key: string): Buffer | undefined {
		const session = this.entries.get(key);
		if (session) {
			//最近使ったものを後ろへ (古いものから捨てる)
			this.entries.delete(key);
			this.entries.set(key, session);
		}
		return session;
	}

	set(key: string, session: Buffer) {
		this.entries.delete(key);
		this.entries.set(key, session);
		if (this.entries.size > MAX_ENTRIES) {
			const oldest = this.entries.keys().next().value;
			if (oldest !== undefined) this.entries.delete(oldest);
		}
	}

	get size(): number {
		return this.entries.size;
	}
}

//信頼する CA が違う接続どうしでセッションを共有しない (再開では証明書の検証が省かれるため)
export function sessionKey(servername: string, port: number, minVersion: string, ca?: TlsCa): string {
	let trust = "system";
	if (ca !== undefined) {
		const hash = createHash("sha256");
		for (const item of Array.isArray(ca) ? ca : [ca]) hash.update(item).update("\0");
		trust = hash.digest("hex");
	}
	return `${servername}:${port}:${minVersion}:${trust}`;
}

export const defaultTlsSessions = new TlsSessionCache();
