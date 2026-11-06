import { randomUUID } from "node:crypto"

//uuid パッケージの置き換え。node:crypto の CSPRNG で v4 UUID を作る
export function uuidv4(): string {
	return randomUUID();
}
