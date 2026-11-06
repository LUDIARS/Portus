import { HandlerRegistry } from "../hot/handlerRegistry.js"
import { parseHandlerRef } from "./router.js"
import { Handler, RouteDef } from "./types.js"

//ルートのハンドラを実際の関数にする。見つからなければ null
export async function resolveHandler(def: RouteDef, registry: HandlerRegistry | null): Promise<Handler | null> {
	if (typeof def.handler == "function") return def.handler;

	const ref = parseHandlerRef(def.handler);
	if (!ref || !registry) return null;

	const mod = await registry.load(ref.module);
	const fn = mod?.[ref.func];
	return typeof fn == "function" ? fn : null;
}
