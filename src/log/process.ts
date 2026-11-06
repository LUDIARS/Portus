import { Logger } from "./logger.js"

//未捕捉の例外・reject をログに残す
//未捕捉例外の後は状態が保証できないので、記録してから終了する (exitOnUncaught=false で継続)
export function installProcessHandlers(log: Logger, exitOnUncaught: boolean = true) {
	process.on("uncaughtException", (err) => {
		log.error("uncaught exception", { err });
		if (!exitOnUncaught) return;
		process.exitCode = 1;
		setImmediate(() => process.exit(1));
	});
	process.on("unhandledRejection", (reason) => {
		log.error("unhandled rejection", { err: reason });
	});
}
