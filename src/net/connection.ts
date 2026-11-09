import * as net from "node:net"
import * as tls from "node:tls"
import { NetError } from "./errors.js"
import { ResolvedAddress } from "./address.js"
import { sessionKey, TlsCa, TlsSessionCache } from "./tlsSessions.js"

export interface ConnectTarget {
	host: string;				//URL のホスト名 (SNI と証明書の照合に使う)
	port: number;
	resolved: ResolvedAddress;	//接続する IP (resolveAddress で固定したもの)
	secure: boolean;
	minTlsVersion: tls.SecureVersion;
	ca?: TlsCa;
	sessions: TlsSessionCache;
}

//TLS の検証失敗 (直らない) と、接続の失敗 (再試行で直りうる) を分ける
function connectError(err: any, host: string): NetError {
	const code = String(err?.code ?? "");
	if (/^ERR_(TLS|SSL)_|CERT|SELF_SIGNED|UNABLE_TO_|HOSTNAME|ALTNAME/.test(code) || err?.library == "SSL routines") {
		return new NetError("TLS", `TLS verification failed for ${host} (${code || err?.message})`);
	}
	return new NetError("CONNECT", `cannot connect to ${host} (${code || err?.message})`);
}

//固定した IP へ接続し、TLS ならハンドシェイクまで終える。signal が切れたら途中でも閉じる
export function openConnection(target: ConnectTarget, signal: AbortSignal): Promise<net.Socket> {
	signal.throwIfAborted();
	return new Promise((resolve, reject) => {
		let socket: net.Socket;
		let key = "";
		if (target.secure) {
			key = sessionKey(target.host, target.port, target.minTlsVersion, target.ca);
			const s = tls.connect({
				host: target.resolved.address,
				port: target.port,
				servername: net.isIP(target.host) ? undefined : target.host,
				ALPNProtocols: ["http/1.1"],
				minVersion: target.minTlsVersion,
				ca: target.ca,
				session: target.sessions.get(key),
			});
			s.on("session", (session: Buffer) => target.sessions.set(key, session));
			socket = s;
		} else {
			socket = net.connect({ host: target.resolved.address, port: target.port });
		}

		const onAbort = () => {
			cleanup();
			socket.destroy();
			reject(signal.reason);
		};
		const onError = (err: Error) => {
			cleanup();
			socket.destroy();
			reject(connectError(err, target.host));
		};
		const onReady = () => {
			cleanup();
			if (socket instanceof tls.TLSSocket) {
				const alpn = socket.alpnProtocol;
				if (alpn && alpn != "http/1.1") {
					socket.destroy();
					reject(new NetError("TLS", `${target.host} negotiated ${alpn} instead of http/1.1`));
					return;
				}
			}
			resolve(socket);
		};
		const cleanup = () => {
			signal.removeEventListener("abort", onAbort);
			socket.off("error", onError);
			socket.off(target.secure ? "secureConnect" : "connect", onReady);
		};

		signal.addEventListener("abort", onAbort, { once: true });
		socket.once("error", onError);
		socket.once(target.secure ? "secureConnect" : "connect", onReady);
	});
}
