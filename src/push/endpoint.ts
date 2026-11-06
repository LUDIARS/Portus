//購読エンドポイントはクライアントが送ってくる URL なので、既知の push サービス以外へは送らない (SSRF 対策)
//先頭 "." はサブドメイン一致
export const DEFAULT_PUSH_HOSTS: readonly string[] = [
	"fcm.googleapis.com",
	"updates.push.services.mozilla.com",
	"web.push.apple.com",
	".push.apple.com",
	".notify.windows.com",
];

export function isAllowedEndpoint(endpoint: string, hosts: readonly string[] = DEFAULT_PUSH_HOSTS): boolean {
	let url: URL;
	try {
		url = new URL(endpoint);
	} catch (ex) {
		return false;
	}
	if (url.protocol != "https:" || url.username || url.password) return false;
	
	const host = url.hostname.toLowerCase();
	return hosts.some((h) => h.startsWith(".") ? host.endsWith(h) : host == h);
}
