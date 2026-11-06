import * as http from "node:http"
import { EventEmitter } from "node:events"
import { Duplex } from "node:stream"
import { buildAcceptResponse, validateUpgrade } from "./handshake.js"
import { DEFAULT_MAX_PAYLOAD, WebSocket } from "./socket.js"

export interface WebSocketServerOptions {
	port: number;
	host?: string;
	maxPayload?: number;
}

//WebSocket サーバ。WebSocket 以外の HTTP 要求には 426 を返す
//  events: connection(ws, req) / listening / close / error
export class WebSocketServer extends EventEmitter {
	private server: http.Server;
	private maxPayload: number;

	constructor(options: WebSocketServerOptions) {
		super();
		this.maxPayload = options.maxPayload ?? DEFAULT_MAX_PAYLOAD;
		this.server = http.createServer((req, res) => {
			res.writeHead(426, { "Content-Type": "text/plain" });
			res.end("Upgrade Required");
		});
		this.server.on("upgrade", (req, socket, head) => this.onUpgrade(req, socket, head));
		this.server.on("listening", () => this.emit("listening"));
		this.server.on("close", () => this.emit("close"));
		this.server.on("error", (err) => this.emit("error", err));
		this.server.listen(options.port, options.host);
	}

	close(callback?: () => void) {
		this.server.close(callback);
	}

	private onUpgrade(req: http.IncomingMessage, socket: Duplex, head: Buffer) {
		//ハンドシェイク前に切断されても落ちないようにする
		socket.on("error", () => socket.destroy());

		const error = validateUpgrade(req.method, req.headers);
		if (error) {
			socket.end(`HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n${error}`);
			return;
		}

		socket.write(buildAcceptResponse(req.headers["sec-websocket-key"] as string));
		const ws = new WebSocket(false, this.maxPayload);
		ws.attach(socket, head);
		this.emit("connection", ws, req);
	}
}
