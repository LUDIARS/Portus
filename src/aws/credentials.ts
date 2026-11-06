import * as fs from "node:fs"
import * as os from "node:os"
import * as path from "node:path"
import { AwsCredentials } from "./sigv4.js"

//aws-sdk v2 の既定チェーンのうち、この環境で使うものだけ:
//  1. 環境変数 AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN
//  2. 共有認証ファイル ~/.aws/credentials (AWS_PROFILE、既定 default)
//  3. EC2 インスタンスメタデータ (IMDSv2)
const IMDS = "http://169.254.169.254";

let cached: AwsCredentials | null = null;

function fromEnv(): AwsCredentials | null {
	const id = process.env.AWS_ACCESS_KEY_ID;
	const secret = process.env.AWS_SECRET_ACCESS_KEY;
	if (!id || !secret) return null;
	return { accessKeyId: id, secretAccessKey: secret, sessionToken: process.env.AWS_SESSION_TOKEN };
}

//ini の該当プロファイルだけを読む
export function parseCredentialsFile(text: string, profile: string): AwsCredentials | null {
	let current = "";
	const values: Record<string, string> = {};
	for (const raw of text.split(/\r?\n/)) {
		const line = raw.trim();
		if (line == "" || line.startsWith("#") || line.startsWith(";")) continue;
		const section = /^\[(.+)\]$/.exec(line);
		if (section) {
			current = section[1]!.trim();
			continue;
		}
		if (current != profile) continue;
		const eq = line.indexOf("=");
		if (eq > 0) values[line.substring(0, eq).trim()] = line.substring(eq + 1).trim();
	}
	if (!values.aws_access_key_id || !values.aws_secret_access_key) return null;
	return { accessKeyId: values.aws_access_key_id, secretAccessKey: values.aws_secret_access_key, sessionToken: values.aws_session_token };
}

function fromSharedFile(): AwsCredentials | null {
	const file = process.env.AWS_SHARED_CREDENTIALS_FILE || path.join(os.homedir(), ".aws", "credentials");
	try {
		return parseCredentialsFile(fs.readFileSync(file, "utf8"), process.env.AWS_PROFILE || "default");
	} catch (ex) {
		return null;
	}
}

async function fromInstanceMetadata(): Promise<AwsCredentials | null> {
	try {
		const tokenRes = await fetch(`${IMDS}/latest/api/token`, {
			method: "PUT",
			headers: { "X-aws-ec2-metadata-token-ttl-seconds": "21600" },
			signal: AbortSignal.timeout(1000),
		});
		if (!tokenRes.ok) return null;
		const token = await tokenRes.text();
		const headers = { "X-aws-ec2-metadata-token": token };

		const roleRes = await fetch(`${IMDS}/latest/meta-data/iam/security-credentials/`, { headers, signal: AbortSignal.timeout(1000) });
		if (!roleRes.ok) return null;
		const role = ((await roleRes.text()).split("\n")[0] ?? "").trim();
		if (!role) return null;

		const credRes = await fetch(`${IMDS}/latest/meta-data/iam/security-credentials/${encodeURIComponent(role)}`, { headers, signal: AbortSignal.timeout(1000) });
		if (!credRes.ok) return null;
		const c: any = await credRes.json();
		return { accessKeyId: c.AccessKeyId, secretAccessKey: c.SecretAccessKey, sessionToken: c.Token, expiration: new Date(c.Expiration) };
	} catch (ex) {
		return null;
	}
}

function isFresh(creds: AwsCredentials): boolean {
	if (!creds.expiration) return true;
	return creds.expiration.valueOf() - Date.now() > 5 * 60 * 1000;
}

export async function resolveCredentials(): Promise<AwsCredentials> {
	if (cached && isFresh(cached)) return cached;

	cached = fromEnv() || fromSharedFile() || await fromInstanceMetadata();
	if (!cached) throw new Error("AWS credentials not found");
	return cached;
}
