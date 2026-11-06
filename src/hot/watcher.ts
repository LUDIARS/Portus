import * as fs from "node:fs"
import * as path from "node:path"

//ディレクトリ直下の .js 変更を、ファイル単位でまとめて通知する(tsc は 1 回の出力で複数回書くため)
export function watchJsFiles(dir: string, onChange: (file: string) => void, debounceMs: number = 150): () => void {
	const timers = new Map<string, NodeJS.Timeout>();

	const watcher = fs.watch(dir, (event, name) => {
		if (!name || !name.toString().endsWith(".js")) return;
		const file = path.join(dir, name.toString());

		const prev = timers.get(file);
		if (prev) clearTimeout(prev);
		timers.set(file, setTimeout(() => {
			timers.delete(file);
			if (fs.existsSync(file)) onChange(file);
		}, debounceMs));
	});

	return () => {
		watcher.close();
		for (const t of timers.values()) clearTimeout(t);
	};
}
