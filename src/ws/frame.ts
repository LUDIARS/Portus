import { randomBytes } from "node:crypto"

//RFC 6455 フレーム。拡張(permessage-deflate 等)は扱わない
export enum OPCODE {
	CONTINUATION = 0x0,
	TEXT = 0x1,
	BINARY = 0x2,
	CLOSE = 0x8,
	PING = 0x9,
	PONG = 0xA,
}

export interface Frame {
	fin: boolean;
	opcode: OPCODE;
	payload: Buffer;
}

export class ProtocolError extends Error {
	constructor(message: string, public closeCode: number) {
		super(message);
	}
}

export function isControl(opcode: number): boolean {
	return (opcode & 0x8) != 0;
}

function applyMask(data: Buffer, mask: Buffer) {
	for (let i = 0; i < data.length; ++i) data[i] = data[i]! ^ mask[i & 3]!;
}

//フレームを組み立てる。クライアント送信時は mask = true
export function encodeFrame(opcode: OPCODE, payload: Buffer, mask: boolean, fin: boolean = true): Buffer {
	const len = payload.length;
	let headerLen = 2 + (mask ? 4 : 0);
	if (len >= 65536) headerLen += 8;
	else if (len >= 126) headerLen += 2;

	const header = Buffer.alloc(headerLen);
	header[0] = (fin ? 0x80 : 0) | opcode;
	let offset = 2;
	if (len >= 65536) {
		header[1] = 127;
		header.writeBigUInt64BE(BigInt(len), 2);
		offset = 10;
	} else if (len >= 126) {
		header[1] = 126;
		header.writeUInt16BE(len, 2);
		offset = 4;
	} else {
		header[1] = len;
	}

	if (!mask) return Buffer.concat([header, payload]);

	header[1] |= 0x80;
	const key = randomBytes(4);
	key.copy(header, offset);
	const body = Buffer.from(payload);
	applyMask(body, key);
	return Buffer.concat([header, body]);
}

//受信バイト列からフレームを取り出す。不正なフレームは ProtocolError を投げる
export class FrameParser {
	private buffer: Buffer = Buffer.alloc(0);

	//expectMasked: サーバ側は true (クライアントは必ずマスクする)、クライアント側は false
	constructor(private expectMasked: boolean, private maxPayload: number) {}

	push(chunk: Buffer): Frame[] {
		this.buffer = this.buffer.length == 0 ? chunk : Buffer.concat([this.buffer, chunk]);
		const frames: Frame[] = [];
		for (;;) {
			const frame = this.next();
			if (!frame) break;
			frames.push(frame);
		}
		return frames;
	}

	private next(): Frame | null {
		const buf = this.buffer;
		if (buf.length < 2) return null;

		const b0 = buf[0]!;
		const b1 = buf[1]!;
		const fin = (b0 & 0x80) != 0;
		if ((b0 & 0x70) != 0) throw new ProtocolError("RSV bits must be 0", 1002);
		const opcode = b0 & 0x0F;
		if (!(opcode in OPCODE)) throw new ProtocolError(`unknown opcode ${opcode}`, 1002);

		const masked = (b1 & 0x80) != 0;
		if (masked != this.expectMasked) throw new ProtocolError(masked ? "unexpected mask" : "frame must be masked", 1002);

		let len = b1 & 0x7F;
		let offset = 2;
		if (len == 126) {
			if (buf.length < 4) return null;
			len = buf.readUInt16BE(2);
			offset = 4;
		} else if (len == 127) {
			if (buf.length < 10) return null;
			const big = buf.readBigUInt64BE(2);
			if (big > BigInt(this.maxPayload)) throw new ProtocolError("payload too large", 1009);
			len = Number(big);
			offset = 10;
		}

		if (isControl(opcode) && (len > 125 || !fin)) throw new ProtocolError("invalid control frame", 1002);
		if (len > this.maxPayload) throw new ProtocolError("payload too large", 1009);

		const maskLen = masked ? 4 : 0;
		if (buf.length < offset + maskLen + len) return null;

		const payload = Buffer.from(buf.subarray(offset + maskLen, offset + maskLen + len));
		if (masked) applyMask(payload, buf.subarray(offset, offset + 4));

		this.buffer = buf.subarray(offset + maskLen + len);
		return { fin, opcode, payload };
	}
}
