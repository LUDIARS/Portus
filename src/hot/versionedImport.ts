import { pathToFileURL } from "node:url"

//ESM は require キャッシュを外せないため、ファイルごとの版番号をクエリに付けて別モジュールとして読み直す
//古い版のモジュールはメモリに残る (開発時のホットリロード専用。本番では版を上げない)
export class VersionedImporter {
	private versions = new Map<string, number>();

	//次の import で新しい版を読むようにする
	bump(file: string) {
		this.versions.set(file, (this.versions.get(file) ?? 0) + 1);
	}

	version(file: string): number {
		return this.versions.get(file) ?? 0;
	}

	async import(file: string): Promise<any> {
		const v = this.version(file);
		const url = pathToFileURL(file).href + (v > 0 ? `?v=${v}` : "");
		return import(url);
	}
}
