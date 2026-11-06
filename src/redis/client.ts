import * as net from "node:net"
import { EventEmitter } from "node:events"
import { encodeCommand, RedisReplyError, RespParser } from "./resp.js"
import { isSafeKey } from "../safeObject.js"

interface Pending {
	resolve: (v: any) => void;
	reject: (e: any) => void;
}

//Redis クライアント (このサーバで使う GET/SET/HSET/HGETALL のみ)
//切断されたら待ち中のコマンドを失敗させ、次のコマンドで再接続する
export class RedisClient extends EventEmitter {
	private socket: net.Socket | null = null;
	private connecting: Promise<void> | null = null;
	private parser = new RespParser();
	private queue: Pending[] = [];

	constructor(private host: string = "127.0.0.1", private port: number = 6379) {
		super();
	}

	connect(): Promise<void> {
		if (this.socket) return Promise.resolve();
		if (this.connecting) return this.connecting;

		this.connecting = new Promise((resolve, reject) => {
			const socket = net.createConnection({ host: this.host, port: this.port });
			socket.setNoDelay(true);
			socket.once("connect", () => {
				this.socket = socket;
				this.connecting = null;
				resolve();
			});
			socket.on("data", (chunk) => this.onData(chunk));
			socket.on("error", (err) => {
				if (this.listenerCount("error") > 0) this.emit("error", err);
				if (this.connecting) {
					this.connecting = null;
					reject(err);
				}
			});
			socket.on("close", () => this.onClose());
		});
		return this.connecting;
	}

	async command(args: string[]): Promise<any> {
		await this.connect();
		return new Promise((resolve, reject) => {
			this.queue.push({ resolve, reject });
			this.socket!.write(encodeCommand(args));
		});
	}

	get(key: string): Promise<string | null> {
		return this.command(["GET", key]);
	}

	set(key: string, value: string): Promise<string> {
		return this.command(["SET", key, value]);
	}

	hSet(key: string, field: string, value: string): Promise<number> {
		return this.command(["HSET", key, field, value]);
	}

	async hGetAll(key: string): Promise<Record<string, string>> {
		const list: string[] = await this.command(["HGETALL", key]);
		const out: Record<string, string> = Object.create(null);
		for (let i = 0; i + 1 < list.length; i += 2) {
			const field = list[i]!;
			if (isSafeKey(field)) out[field] = list[i + 1]!;
		}
		return out;
	}

	quit() {
		this.socket?.end();
	}

	private onData(chunk: Buffer) {
		let replies: any[];
		try {
			replies = this.parser.push(chunk);
		} catch (ex) {
			//解析不能な応答が来たら接続ごと捨てる
			this.socket?.destroy(ex as Error);
			return;
		}
		for (const reply of replies) {
			const p = this.queue.shift();
			if (!p) continue;
			if (reply instanceof RedisReplyError) p.reject(reply);
			else p.resolve(reply);
		}
	}

	private onClose() {
		this.socket = null;
		this.parser = new RespParser();
		const pending = this.queue;
		this.queue = [];
		for (const p of pending) p.reject(new Error("redis connection closed"));
	}
}
