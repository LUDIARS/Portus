//接続先ごとの同時実行数の上限 (QUIC の max_streams にあたる)。
//上限に達したら順番待ち (到着順)。待ちは呼び出し側の期限・中断で抜ける
export class ConcurrencyLimiter {
	private active = new Map<string, number>();
	private waiting = new Map<string, Array<() => void>>();

	constructor(private limit: number) {
		if (!Number.isInteger(limit) || limit < 1) throw new Error("invalid concurrency limit");
	}

	//空きを待って取る。返り値は 1 回だけ効く解放関数
	acquire(key: string, signal: AbortSignal): Promise<() => void> {
		signal.throwIfAborted();
		const count = this.active.get(key) ?? 0;
		if (count < this.limit) {
			this.active.set(key, count + 1);
			return Promise.resolve(this.releaser(key));
		}
		return new Promise((resolve, reject) => {
			const queue = this.waiting.get(key) ?? [];
			const grant = () => {
				signal.removeEventListener("abort", onAbort);
				resolve(this.releaser(key));
			};
			const onAbort = () => {
				const i = queue.indexOf(grant);
				if (i >= 0) queue.splice(i, 1);
				if (queue.length == 0) this.waiting.delete(key);
				reject(signal.reason);
			};
			queue.push(grant);
			this.waiting.set(key, queue);
			signal.addEventListener("abort", onAbort, { once: true });
		});
	}

	inUse(key: string): number {
		return this.active.get(key) ?? 0;
	}

	private releaser(key: string): () => void {
		let released = false;
		return () => {
			if (released) return;
			released = true;
			const queue = this.waiting.get(key);
			const next = queue?.shift();
			if (queue && queue.length == 0) this.waiting.delete(key);
			if (next) {
				next();	//枠をそのまま次へ渡す
				return;
			}
			const count = (this.active.get(key) ?? 1) - 1;
			if (count <= 0) this.active.delete(key);
			else this.active.set(key, count);
		};
	}
}
