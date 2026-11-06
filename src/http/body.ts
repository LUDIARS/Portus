import { IncomingMessage } from "node:http"
import { assignSafe } from "../safeObject.js"
import { parseQueryString } from "./router.js"

export class BodyTooLargeError extends Error {
	constructor(public limit: number) {
		super(`request body exceeds ${limit} bytes`);
	}
}

//リクエストボディを上限付きで読む
export function readBody(req: IncomingMessage, limit: number): Promise<string> {
	return new Promise((resolve, reject) => {
		const declared = Number(req.headers["content-length"] || 0);
		if (declared > limit) {
			req.resume();
			return reject(new BodyTooLargeError(limit));
		}

		const chunks: Buffer[] = [];
		let size = 0;
		req.on("data", (chunk: Buffer) => {
			size += chunk.length;
			if (size > limit) {
				req.destroy();
				reject(new BodyTooLargeError(limit));
				return;
			}
			chunks.push(chunk);
		});
		req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
		req.on("error", reject);
	});
}

//JSON を優先し、だめなら x-www-form-urlencoded として解釈して dst へ足す
export function mergeBody(dst: Record<string, any>, text: string): boolean {
	if (text == "") return true;
	try {
		assignSafe(dst, JSON.parse(text));
		return true;
	} catch (ex) {
		//JSON でなければフォーム形式を試す
	}
	try {
		assignSafe(dst, parseQueryString(text));
		return true;
	} catch (ex) {
		return false;
	}
}
