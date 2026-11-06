import { format } from "node:util"
import { Logger } from "./logger.js"

//既存コードの console.* をロガーへ流す。呼び出し側を書き換えずにレベル・時刻・出力先を揃えるため
export function installConsoleBridge(logger: Logger) {
	console.debug = (...args: any[]) => logger.debug(format(...args));
	console.log = (...args: any[]) => logger.info(format(...args));
	console.info = (...args: any[]) => logger.info(format(...args));
	console.warn = (...args: any[]) => logger.warn(format(...args));
	console.error = (...args: any[]) => logger.error(format(...args));
	console.trace = (...args: any[]) => logger.debug(format(...args), { stack: new Error().stack });
}
