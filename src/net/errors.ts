//通信層のエラー。code で原因を分ける (再試行するかどうかは呼び出し側が code で決める)
export type NetErrorCode =
	| "ADDRESS"		//名前解決できない、または許可されていないアドレスにしか解決しない
	| "CONNECT"		//接続できない (拒否・到達不能・切断)
	| "TLS"			//証明書・TLS の版・ALPN の検証失敗 (再試行しても直らない)
	| "PROTOCOL"	//応答の形が規約外 (圧縮・長さの無い本文・不正なヘッダなど)
	| "LIMIT"		//大きさ・数の上限を超えた
	| "CLOSED"		//応答の途中で接続が切れた
	| "TIMEOUT"
	| "ABORTED";

export class NetError extends Error {
	constructor(public code: NetErrorCode, message: string) {
		super(message);
		//標準の AbortSignal と同じ名前にそろえる (呼び出し側が name で判定できるように)
		this.name = code == "TIMEOUT" ? "TimeoutError" : code == "ABORTED" ? "AbortError" : "NetError";
	}
}

//2xx 以外の応答。body はエラー本文の先頭だけ (切り詰め済み)
export class HttpError extends Error {
	constructor(message: string, public status: number, public body: string) {
		super(message);
	}
}
