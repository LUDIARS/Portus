import { isSafeKey } from "../safeObject.js"
import { Handler, RouteDef } from "./types.js"

type Segment = { literal: string } | { param: string, type: "string" | "number" };

interface CompiledRoute {
	def: RouteDef;
	segments: Segment[];
}

export interface RouteMatch {
	def: RouteDef;
	params: Record<string, any>;
	query: Record<string, any>;
}

//"モジュール#関数" を分解する。関数ハンドラなら null
export function parseHandlerRef(handler: string | Handler): { module: string, func: string } | null {
	if (typeof handler == "function") return null;
	const m = /^([A-Za-z0-9_]+)#([A-Za-z0-9_]+)$/.exec(handler);
	if (!m) throw new Error(`invalid handler "${handler}" (expected "module#function")`);
	return { module: m[1]!, func: m[2]! };
}

const NUMBER_VALUE = /^-?[0-9]+(\.[0-9]+)?$/;

function splitPath(path: string): string[] {
	return path.split("/").filter((s) => s != "");
}

function compileSegment(seg: string): Segment {
	const m = /^\{([A-Za-z_][A-Za-z0-9_]*)(?::(string|number))?\}$/.exec(seg);
	if (!m) return { literal: seg };
	return { param: m[1]!, type: (m[2] || "string") as "string" | "number" };
}

function compile(def: RouteDef): CompiledRoute {
	parseHandlerRef(def.handler);	//形式チェック
	return { def, segments: splitPath(def.path).map(compileSegment) };
}

//クエリ文字列を null プロトタイプのオブジェクトへ
export function parseQueryString(qs: string): Record<string, any> {
	const out: Record<string, any> = Object.create(null);
	for (const [k, v] of new URLSearchParams(qs)) {
		if (isSafeKey(k)) out[k] = v;
	}
	return out;
}

function matchSegments(route: CompiledRoute, parts: string[], vars: Record<string, any>): boolean {
	if (route.segments.length != parts.length) return false;
	for (let i = 0; i < parts.length; ++i) {
		const seg = route.segments[i]!;
		let value: string;
		try {
			value = decodeURIComponent(parts[i]!);
		} catch (ex) {
			return false;
		}

		if ("literal" in seg) {
			if (seg.literal != value) return false;
		} else if (seg.type == "number") {
			if (!NUMBER_VALUE.test(value)) return false;
			vars[seg.param] = Number(value);
		} else {
			vars[seg.param] = value;
		}
	}
	return true;
}

//表駆動のルータ。表に書いたものだけが一致し、親ルートへのフォールバックはしない
export class Router {
	private routes: CompiledRoute[];

	constructor(defs: RouteDef[]) {
		this.routes = defs.map(compile);
	}

	//URL (パス + クエリ) を解決する。見つからなければ null
	match(method: string, url: string): RouteMatch | null {
		const q = url.indexOf("?");
		const parts = splitPath(q >= 0 ? url.substring(0, q) : url);

		for (const route of this.routes) {
			if (route.def.method != method) continue;

			const params: Record<string, any> = Object.create(null);
			if (!matchSegments(route, parts, params)) continue;

			const query = q >= 0 ? parseQueryString(url.substring(q + 1)) : Object.create(null);
			Object.assign(query, params);
			return { def: route.def, params, query };
		}
		return null;
	}

	//ルート表をそのまま返す (一覧 API などで公開できるように)
	list(): RouteDef[] {
		return this.routes.map((r) => r.def);
	}

	//"モジュール#関数" で参照されるモジュール名の一覧
	modules(): string[] {
		const names = new Set<string>();
		for (const r of this.routes) {
			const ref = parseHandlerRef(r.def.handler);
			if (ref) names.add(ref.module);
		}
		return Array.from(names);
	}
}
