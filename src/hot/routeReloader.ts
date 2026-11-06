import * as path from "node:path"
import { Logger } from "../log/index.js"
import { VersionedImporter } from "./versionedImport.js"
import { watchJsFiles } from "./watcher.js"

//ルート表モジュール (export const routes) の変更を監視し、読み直せたら apply に渡す
//読み直しに失敗したら apply しない (壊れた表で動かさず、前の表のまま続ける)
export function watchRouteModule(file: string, exportName: string, apply: (routes: any) => void, log?: Logger): () => void {
	const importer = new VersionedImporter();
	const target = path.resolve(file);

	return watchJsFiles(path.dirname(target), async (changed) => {
		if (path.resolve(changed) != target) return;
		importer.bump(target);
		try {
			const mod = await importer.import(target);
			const routes = mod[exportName];
			if (!Array.isArray(routes)) throw new Error(`${exportName} is not an array`);
			apply(routes);
			log?.info("routes reloaded", { count: routes.length });
		} catch (ex) {
			log?.error("route reload failed", { err: ex });
		}
	});
}
