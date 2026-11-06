import { HandlerResult, Middleware, RequestContext } from "./types.js"

//ミドルウェアを外側から順に重ね、最後に final を呼ぶ
export function compose(middlewares: Middleware[], final: (ctx: RequestContext) => Promise<HandlerResult>) {
	return (ctx: RequestContext): Promise<HandlerResult> => {
		let index = -1;
		const run = (i: number): Promise<HandlerResult> => {
			if (i <= index) return Promise.reject(new Error("next() called multiple times"));
			index = i;
			const mw = middlewares[i];
			if (!mw) return final(ctx);
			return mw(ctx, () => run(i + 1));
		};
		return run(0);
	};
}
