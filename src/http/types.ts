import { IncomingMessage, ServerResponse } from "node:http"
import { Logger } from "../log/index.js"
import { Reply } from "./reply.js"

export type HttpMethod = "GET" | "POST" | "PUT" | "PATCH" | "DELETE";

//ルート定義。表に書いたものだけが到達可能で、上から順に最初に一致したものを使う
//  path:    {name} は文字列変数、{name:number} は数値変数 (整数・小数)
//  handler: 関数、または "モジュール#関数" (handlersDir/<モジュール>.js の export 関数。ホットリロード対象)
export interface RouteDef {
	method: HttpMethod;
	path: string;
	handler: string | Handler;
	note?: string;
	meta?: Record<string, any>;	//ミドルウェアが参照する任意の印 (認証要否など)
}

//1 リクエストの文脈
export interface RequestContext {
	req: IncomingMessage;
	res: ServerResponse;
	reqId: string;
	method: string;
	path: string;
	route: RouteDef;
	params: Record<string, any>;	//パス変数だけ
	query: Record<string, any>;		//クエリ + パス変数 + ボディ (JSON / フォーム) を合わせたもの。null プロトタイプ
	body: string;					//生のボディ (GET は空)
	state: Record<string, any>;		//ミドルウェア間の受け渡し
	log: Logger;
}

//ハンドラの戻り値
//  Reply → そのまま / undefined・null → 204 / string → text / Buffer → octet-stream / それ以外 → JSON 200
export type HandlerResult = Reply | object | string | Buffer | null | undefined | void;

export type Handler = (ctx: RequestContext) => HandlerResult | Promise<HandlerResult>;

//next() でハンドラ(または次のミドルウェア)を呼ぶ。呼ばずに Reply を返せば打ち切り
export type Middleware = (ctx: RequestContext, next: () => Promise<HandlerResult>) => Promise<HandlerResult>;
