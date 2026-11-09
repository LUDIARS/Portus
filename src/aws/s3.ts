import { request } from "../net/request.js"
import { resolveCredentials } from "./credentials.js"
import { encodeKeyPath, signRequest } from "./sigv4.js"

//aws-sdk の s3.putObject 置き換え (PutObject のみ)
export async function putObject(region: string, bucket: string, key: string, body: string | Buffer, contentType: string = "text/plain; charset=utf-8") {
	if (!/^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$/.test(bucket)) throw new Error(`invalid bucket name ${bucket}`);

	const payload = typeof body == "string" ? Buffer.from(body, "utf8") : body;
	const host = `${bucket}.s3.${region}.amazonaws.com`;
	const path = "/" + encodeKeyPath(key);

	const headers = signRequest({
		method: "PUT",
		host,
		path,
		region,
		service: "s3",
		payload,
		headers: { "content-type": contentType },
	}, await resolveCredentials());

	const res = await request(`https://${host}${path}`, {
		method: "PUT",
		headers,
		body: payload,
		timeoutMs: 30000,
		minTlsVersion: "TLSv1.2",	//S3 のエンドポイントが TLS 1.3 を出すとは限らない
	});
	if (res.status < 200 || res.status >= 300) {
		const text = await res.text(300);
		throw new Error(`S3 PutObject failed ${res.status}: ${text}`);
	}
	res.close();
}
