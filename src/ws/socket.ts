import { EventEmitter } from "node:events"
import { Duplex } from "node:stream"
import { TextDecoder } from "node:util"
import { encodeFrame, Frame, FrameParser, isControl, OPCODE, ProtocolError } from "./frame.js"

export const DEFAULT_MAX_PAYLOAD = 1024 * 1024;

const utf8 = new TextDecoder("utf-8", { fatal: true });

//WebSocket 接続 1 本。ws パッケージで使っていた範囲の API だけを持つ
//  events: open / message(data: Buffer, isBinary) / ping / pong / close(code, reason: Buffer) / error
export class WebSocket extends EventEmitter {
	static readonly CONNECTING = 0;
	static readonly OPEN = 1;
	static readonly CLOSING = 2;
	static readonly CLOSED = 3;

	readyState: number = WebSocket.CONNECTING;

	private socket: Duplex | null = null;
	private parser: FrameParser;
	private fragments: Buffer[] = [];
	private fragmentOpcode: OPCODE | null = null;
	private fragmentSize = 0;
	private closeCode = 1006;
	private closeReason: Buffer = Buffer.alloc(0);

	//isClient: 自分がクライアントか(送信フレームをマスクする側か)
	constructor(private isClient: boolean, private maxPayload: number = DEFAULT_MAX_PAYLOAD) {
		super();
		this.parser = new FrameParser(!isClient, maxPayload);
	}

	//ハンドシェイク済みのソケットを結び付けて OPEN にする
	attach(socket: Duplex, head?: Buffer) {
		this.socket = socket;
		this.readyState = WebSocket.OPEN;

		socket.on("data", (chunk: Buffer) => this.onData(chunk));
		socket.on("close", () => this.onSocketClose());
		socket.on("error", (err) => this.emitError(err));

		this.emit("open");
		if (head && head.length > 0) this.onData(head);
	}

	send(data: string | Buffer) {
		if (this.readyState != WebSocket.OPEN) return;
		const isText = typeof data == "string";
		this.write(isText ? OPCODE.TEXT : OPCODE.BINARY, isText ? Buffer.from(data, "utf8") : data);
	}

	ping(data: Buffer = Buffer.alloc(0)) {
		if (this.readyState != WebSocket.OPEN) return;
		this.write(OPCODE.PING, data.subarray(0, 125));
	}

	close(code: number = 1000, reason: string = "") {
		if (this.readyState != WebSocket.OPEN) return;
		const body = Buffer.alloc(2 + Buffer.byteLength(reason));
		body.writeUInt16BE(code, 0);
		body.write(reason, 2);
		this.write(OPCODE.CLOSE, body.subarray(0, 125));
		this.readyState = WebSocket.CLOSING;
		//相手の応答が来なくても閉じる
		setTimeout(() => this.terminate(), 5000).unref();
	}

	terminate() {
		if (this.readyState == WebSocket.CLOSED) return;
		this.readyState = WebSocket.CLOSING;
		this.socket?.destroy();
	}

	private write(opcode: OPCODE, payload: Buffer) {
		this.socket?.write(encodeFrame(opcode, payload, this.isClient));
	}

	private onData(chunk: Buffer) {
		try {
			for (const frame of this.parser.push(chunk)) this.onFrame(frame);
		} catch (ex) {
			this.failConnection(ex);
		}
	}

	private onFrame(frame: Frame) {
		if (isControl(frame.opcode)) return this.onControl(frame);

		if (frame.opcode == OPCODE.CONTINUATION) {
			if (this.fragmentOpcode == null) throw new ProtocolError("unexpected continuation", 1002);
		} else {
			if (this.fragmentOpcode != null) throw new ProtocolError("expected continuation", 1002);
			this.fragmentOpcode = frame.opcode;
		}

		this.fragmentSize += frame.payload.length;
		if (this.fragmentSize > this.maxPayload) throw new ProtocolError("message too large", 1009);
		this.fragments.push(frame.payload);
		if (!frame.fin) return;

		const data = Buffer.concat(this.fragments);
		const isBinary = this.fragmentOpcode == OPCODE.BINARY;
		this.fragments = [];
		this.fragmentOpcode = null;
		this.fragmentSize = 0;

		if (!isBinary) {
			try {
				utf8.decode(data);
			} catch (ex) {
				throw new ProtocolError("invalid utf-8", 1007);
			}
		}
		this.emit("message", data, isBinary);
	}

	private onControl(frame: Frame) {
		switch (frame.opcode) {
		case OPCODE.PING:
			this.write(OPCODE.PONG, frame.payload);
			this.emit("ping", frame.payload);
			break;

		case OPCODE.PONG:
			this.emit("pong", frame.payload);
			break;

		case OPCODE.CLOSE:
			this.closeCode = frame.payload.length >= 2 ? frame.payload.readUInt16BE(0) : 1005;
			this.closeReason = frame.payload.subarray(2);
			if (this.readyState == WebSocket.OPEN) {
				//受けた close をそのまま返して閉じる
				this.write(OPCODE.CLOSE, frame.payload.subarray(0, 2));
				this.readyState = WebSocket.CLOSING;
			}
			this.socket?.end();
			break;
		}
	}

	private failConnection(ex: any) {
		const code = ex instanceof ProtocolError ? ex.closeCode : 1002;
		this.emitError(ex);
		if (this.readyState == WebSocket.OPEN) {
			const body = Buffer.alloc(2);
			body.writeUInt16BE(code, 0);
			this.write(OPCODE.CLOSE, body);
		}
		this.closeCode = code;
		this.terminate();
	}

	//リスナーが無い error でプロセスを落とさない
	private emitError(err: any) {
		if (this.listenerCount("error") > 0) this.emit("error", err);
	}
	
	private onSocketClose() {
		if (this.readyState == WebSocket.CLOSED) return;
		this.readyState = WebSocket.CLOSED;
		this.emit("close", this.closeCode, this.closeReason);
	}
}
