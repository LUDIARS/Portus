import * as http from "node:http"
import { AddressInfo } from "node:net"
import { HandlerRegistry } from "../hot/handlerRegistry.js"
import { watchRouteModule } from "../hot/routeReloader.js"
import { uuidv4 } from "../id.js"
import { createRootLogger, Logger } from "../log/index.js"
import { BodyTooLargeError, mergeBody, readBody } from "./body.js"
import { resolveHandler } from "./handlerResolver.js"
import { compose } from "./middleware.js"
import { sendReply, text, toReply } from "./reply.js"
import { RouteMatch, Router } from "./router.js"
import { HandlerResult, Middleware, RequestContext, RouteDef } from "./types.js"

export interface HttpServerOptions {
	routes: RouteDef[];
	handlersDir?: string;		//"モジュール#関数" ハンドラの置き場所 (ビルド後の .js)
	middlewares?: Middleware[];
	maxBodyBytes?: number;		//既定 1MiB。超えたら 413
	log?: Logger;
	hotReload?: boolean;		//handlersDir の変更を次のリクエストから反映する
	routesFile?: string;		//hotReload 時、このモジュールの routesExport を監視して表を差し替える
	routesExport?: string;		//既定 "routes"
}

export interface PortusHttpServer {
	server: http.Server;
	listen(port: number, host?: string): Promise<AddressInfo>;
	setRoutes(routes: RouteDef[]): void;
	routes(): RouteDef[];
	close(): Promise<void>;
}

const DEFAULT_MAX_BODY = 1024 * 1024;
const BODY_METHODS = new Set(["POST", "PUT", "PATCH", "DELETE"]);

export function createHttpServer(options: HttpServerOptions): PortusHttpServer {
	const log = options.log ?? createRootLogger("http", "info", []);
	const maxBody = options.maxBodyBytes ?? DEFAULT_MAX_BODY;
	let router = new Router(options.routes);
	const registry = options.handlersDir ? new HandlerRegistry(options.handlersDir, router.modules(), log) : null;
	const stops: (() => void)[] = [];

	const callHandler = async (ctx: RequestContext): Promise<HandlerResult> => {
		const handler = await resolveHandler(ctx.route, registry);
		if (!handler) return text("handler not found.", 404);
		return handler(ctx);
	};
	const pipeline = compose(options.middlewares ?? [], callHandler);

	async function buildContext(req: http.IncomingMessage, res: http.ServerResponse, reqId: string, path: string, match: RouteMatch): Promise<RequestContext> {
		const body = BODY_METHODS.has(req.method || "") ? await readBody(req, maxBody) : "";
		mergeBody(match.query, body);
		return {
			req, res, reqId, path, body,
			method: req.method || "",
			route: match.def,
			params: match.params,
			query: match.query,
			state: Object.create(null),
			log: log.child("req", { reqId }),
		};
	}

	async function handle(req: http.IncomingMessage, res: http.ServerResponse) {
		const started = Date.now();
		const reqId = uuidv4();
		const method = req.method || "";
		const path = (req.url || "/").split("?")[0]!;
		res.setHeader("x-request-id", reqId);

		let status = 500;
		let handler = "-";
		try {
			const match = router.match(method, req.url || "/");
			if (!match) {
				status = 404;
				sendReply(res, text("not found.", 404));
			} else {
				handler = typeof match.def.handler == "string" ? match.def.handler : (match.def.handler.name || "fn");
				const ctx = await buildContext(req, res, reqId, path, match);
				const reply = toReply(await pipeline(ctx));
				status = reply.status;
				sendReply(res, reply);
			}
		} catch (ex) {
			status = ex instanceof BodyTooLargeError ? 413 : 500;
			if (status == 500) log.error("handler failed", { reqId, method, path, handler, err: ex });
			sendReply(res, text(status == 413 ? "payload too large." : "internal error.", status));
		}

		log.info("access", { reqId, method, path, handler, status, ms: Date.now() - started });
	}

	const server = http.createServer((req, res) => {
		void handle(req, res);
	});
	server.on("clientError", (err, socket) => socket.destroy());

	const setRoutes = (routes: RouteDef[]) => {
		const next = new Router(routes);
		router = next;
		registry?.setModules(next.modules());
	};

	if (options.hotReload) {
		if (registry) {
			registry.enableHotReload();
			stops.push(() => registry.disableHotReload());
		}
		if (options.routesFile) {
			stops.push(watchRouteModule(options.routesFile, options.routesExport ?? "routes", setRoutes, log));
		}
	}

	return {
		server,
		setRoutes,
		routes: () => router.list(),
		listen(port: number, host: string = "127.0.0.1") {
			return new Promise((resolve, reject) => {
				server.once("error", reject);
				server.listen(port, host, () => {
					server.off("error", reject);
					const address = server.address() as AddressInfo;
					log.info("http server listening", { host, port: address.port, routes: router.list().length });
					resolve(address);
				});
			});
		},
		close() {
			for (const stop of stops) stop();
			return new Promise((resolve) => server.close(() => resolve()));
		},
	};
}
