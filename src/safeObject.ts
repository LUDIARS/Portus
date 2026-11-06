//外部入力のキーをオブジェクトへ移すときに、プロトタイプ汚染につながるキーを落とす
const FORBIDDEN_KEYS = new Set(["__proto__", "constructor", "prototype"]);

export function isSafeKey(key: string): boolean {
	return !FORBIDDEN_KEYS.has(key);
}

//src の自前キーを dst へコピーする(危険なキーは無視)
export function assignSafe(dst: any, src: any) {
	if (src == null || typeof src != "object") return dst;
	for (const key of Object.keys(src)) {
		if (!isSafeKey(key)) continue;
		dst[key] = src[key];
	}
	return dst;
}
