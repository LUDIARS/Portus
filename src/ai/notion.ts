import { fetchJson } from "../net/fetchJson.js"

const BASE = "https://api.notion.com/v1";
const VERSION = "2022-06-28";

function id(value: string): string {
	if (typeof value != "string" || !/^[0-9a-fA-F-]{32,36}$/.test(value)) throw new Error("invalid notion id");
	return value;
}

function cursorQuery(cursor?: string): string {
	return cursor ? `?start_cursor=${encodeURIComponent(cursor)}` : "";
}

//@notionhq/client の置き換え。このサーバで呼んでいるメソッドだけを同じ名前で持つ
export class NotionClient {
	constructor(private token: string | undefined) {}

	private call(path: string, body?: any) {
		return fetchJson(BASE + path, {
			method: body === undefined ? "GET" : "POST",
			headers: { "Authorization": `Bearer ${this.token || ""}`, "Notion-Version": VERSION },
			body,
		});
	}

	users = {
		list: (p: { start_cursor?: string }) => this.call(`/users${cursorQuery(p.start_cursor)}`),
	};

	databases = {
		query: (p: { database_id: string, filter?: any, start_cursor?: string }) => {
			const { database_id, ...body } = p;
			return this.call(`/databases/${id(database_id)}/query`, body);
		},
	};

	blocks = {
		children: {
			list: (p: { block_id: string, start_cursor?: string }) => this.call(`/blocks/${id(p.block_id)}/children${cursorQuery(p.start_cursor)}`),
		},
	};

	pages = {
		retrieve: (p: { page_id: string }) => this.call(`/pages/${id(p.page_id)}`),
		create: (data: any) => this.call(`/pages`, data),
	};

	search = (p: any) => this.call(`/search`, p);
}
