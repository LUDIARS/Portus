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

	const res = await fetch(`https://${host}${path}`, {
		method: "PUT",
		headers,
		body: payload,
		redirect: "error",
		signal: AbortSignal.timeout(30000),
	});
	if (!res.ok) {
		const text = (await res.text()).substring(0, 300);
		throw new Error(`S3 PutObject failed ${res.status}: ${text}`);
	}
}
