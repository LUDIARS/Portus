import { lookup } from "node:dns/promises"
import { BlockList, isIP } from "node:net"
import { NetError } from "./errors.js"

//内部向けのアドレス。既定では接続しない (SSRF 対策)。IPv4 射影の IPv6 (::ffff:a.b.c.d) も IPv4 の規則で判定される
const INTERNAL = new BlockList();
for (const [network, prefix] of [
	["0.0.0.0", 8], ["10.0.0.0", 8], ["100.64.0.0", 10], ["127.0.0.0", 8], ["169.254.0.0", 16], ["172.16.0.0", 12],
	["192.0.0.0", 24], ["192.168.0.0", 16], ["198.18.0.0", 15], ["224.0.0.0", 4], ["240.0.0.0", 4],
] as const) INTERNAL.addSubnet(network, prefix, "ipv4");
for (const [network, prefix] of [["::", 128], ["::1", 128], ["fc00::", 7], ["fe80::", 10], ["ff00::", 8]] as const) INTERNAL.addSubnet(network, prefix, "ipv6");

export function isInternalAddress(ip: string): boolean {
	const family = isIP(ip);
	if (family == 4) return INTERNAL.check(ip, "ipv4");
	if (family == 6) return INTERNAL.check(ip, "ipv6");
	return true;	//IP でないものは接続先にしない
}

export interface ResolvedAddress {
	address: string;
	family: 4 | 6;
}

//名前解決は 1 回だけ。ここで選んだ IP に接続し、以後は解決し直さない (DNS の差し替えで行き先を変えられないように)
export async function resolveAddress(host: string, allowInternal: boolean): Promise<ResolvedAddress> {
	const literal = isIP(host);
	let candidates: ResolvedAddress[];
	if (literal) {
		candidates = [{ address: host, family: literal as 4 | 6 }];
	} else {
		try {
			candidates = (await lookup(host, { all: true, verbatim: true })) as ResolvedAddress[];
		} catch (ex) {
			throw new NetError("ADDRESS", `cannot resolve ${host}`);
		}
	}
	for (const c of candidates) {
		if (allowInternal || !isInternalAddress(c.address)) return c;
	}
	throw new NetError("ADDRESS", `${host} resolves only to internal addresses`);
}
