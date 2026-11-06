import * as path from "node:path"
import { Logger } from "../log/index.js"
import { VersionedImporter } from "./versionedImport.js"
import { watchJsFiles } from "./watcher.js"

const MODULE_NAME = /^[A-Za-z0-9_]+$/;

//"モジュール#関数" 形式のハンドラを <dir>/<モジュール>.js から読む
//読めるのは allowed に載せたモジュール名だけ (ルート表に無いファイルは読まない)
//ホットリロード有効時は、ファイルが書き換わったら次の load で新しい版を読む
export class HandlerRegistry {
	private allowed: Set<string>;
	private importer = new VersionedImporter();
	private stopWatch: (() => void) | null = null;

	constructor(private dir: string, modules: Iterable<string>, private log?: Logger) {
		this.allowed = new Set(modules);
	}

	setModules(modules: Iterable<string>) {
		this.allowed = new Set(modules);
	}

	fileOf(name: string): string {
		return path.join(this.dir, `${name}.js`);
	}

	async load(name: string): Promise<any> {
		if (!MODULE_NAME.test(name) || !this.allowed.has(name)) return null;
		return this.importer.import(this.fileOf(name));
	}

	enableHotReload() {
		if (this.stopWatch) return;
		this.stopWatch = watchJsFiles(this.dir, (file) => {
			this.importer.bump(file);
			this.log?.info("handler reloaded", { file: path.basename(file) });
		});
		this.log?.info("hot reload enabled", { dir: this.dir });
	}

	disableHotReload() {
		this.stopWatch?.();
		this.stopWatch = null;
	}
}
