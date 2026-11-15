// Wire format shared with the C++ engine (engine/include/Protocol.hpp). Keep the two in sync.
//
//   frame = u32 length | u8 type | body          (all numbers little-endian)
//   length counts the type byte plus the body and must be 1..1024.
//
//   1  NEW_ORDER    body 32:  side u8, orderType u8, symbol u16, user u32,
//                             price i64, qty i64, clientRef u64
//   2  CANCEL_ORDER body 22:  symbol u16, user u32, orderId u64, clientRef u64
//   16 EVENT        body 108: see decodeEvent
//
// IDs (orderId, clientRef, tradeId, seq) are bigint. JSON cannot serialize a bigint, so
// convert them to strings before sending them to a browser. Prices and quantities are
// numbers; the engine caps them far below 2^53, and anything larger throws a RangeError.

export const Side = Object.freeze({ BUY: 0, SELL: 1 });
export const OrderType = Object.freeze({ LIMIT: 0, MARKET: 1 });
export const MsgType = Object.freeze({
  NEW_ORDER: 1,
  CANCEL_ORDER: 2,
  QUERY_ACCOUNT: 3,
  QUERY_BOOK: 4,
  QUERY_INFO: 5,
  EVENT: 16,
});
export const EventType = Object.freeze({
  ORDER_ACCEPTED: 0,
  ORDER_REJECTED: 1,
  TRADE: 2,
  ORDER_CANCELLED: 3,
  CANCEL_REJECTED: 4,
  // Query answers: a run of events with the query's clientRef, ending in QUERY_DONE.
  ACCOUNT_BALANCE: 5, // user, price = cash (paise), qty = cash reserved by open buys
  ACCOUNT_POSITION: 6, // user, symbol, qty = shares owned, remaining = shares reserved, price = total cost basis
  BOOK_LEVEL: 7, // symbol, side, price, qty = total shares at that price (bids then asks, best first)
  QUERY_DONE: 8,
  QUERY_REJECTED: 9, // reason = UNKNOWN_USER or UNKNOWN_SYMBOL
  ENGINE_INFO: 10, // qty = the engine's journal id (0 = none), remaining = number of symbols
});
export const OrderStatus = Object.freeze({
  NEW: 0,
  PARTIAL: 1,
  FILLED: 2,
  CANCELLED: 3,
  REJECTED: 4,
});
export const RejectReason = Object.freeze({
  NONE: 0,
  UNKNOWN_USER: 1,
  UNKNOWN_SYMBOL: 2,
  BAD_QUANTITY: 3,
  BAD_PRICE: 4,
  INSUFFICIENT_FUNDS: 5,
  INSUFFICIENT_SHARES: 6,
  PRICE_OFF_TICK: 7,
  PRICE_OUT_OF_BAND: 8,
});

export const MAX_FRAME_LENGTH = 1024;
export const EVENT_BODY = 108;
const LEN_BYTES = 4;
const NEW_ORDER_BODY = 32;
const CANCEL_BODY = 22;
const QUERY_ACCOUNT_BODY = 12;
const QUERY_BOOK_BODY = 14;
const QUERY_INFO_BODY = 8;

export class ProtocolError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ProtocolError';
  }
}

// ---- order ids: the symbol lives in the top 16 bits ----

const ID_SHIFT = 48n;
const LOCAL_MASK = (1n << ID_SHIFT) - 1n;

/** @param {number} symbol @param {bigint|number} local @returns {bigint} */
export const makeOrderId = (symbol, local) => (BigInt(symbol) << ID_SHIFT) | BigInt(local);
/** @param {bigint} id @returns {number} */
export const symbolOfOrderId = (id) => Number(id >> ID_SHIFT);
/** @param {bigint} id @returns {bigint} */
export const localOrderId = (id) => id & LOCAL_MASK;

// ---- commands (gateway -> engine) ----

/**
 * A command is a plain object, one of:
 *   { kind: 'new',    side, orderType, symbol, user, price, qty, clientRef }
 *   { kind: 'cancel', symbol, user, orderId, clientRef }
 * side and orderType use the Side and OrderType constants. price (in paise) and qty are
 * numbers; orderId and clientRef are bigint.
 *
 * @typedef {Object} Command
 */

function checkUint(value, max, what) {
  if (!Number.isInteger(value) || value < 0 || value > max) {
    throw new RangeError(`${what} must be an integer in 0..${max}, got ${value}`);
  }
  return value;
}

function checkBigint(value, what) {
  if (typeof value !== 'bigint') {
    throw new TypeError(`${what} must be a bigint, got ${typeof value}`);
  }
  return value;
}

function writeI64(buf, value, offset, what) {
  if (!Number.isSafeInteger(value)) {
    throw new RangeError(`${what} must be a safe integer, got ${value}`);
  }
  buf.writeBigInt64LE(BigInt(value), offset);
}

/**
 * Returns one complete frame, ready to write to the socket.
 * Throws RangeError or TypeError on bad values, so a mistake never reaches the engine
 * (which would answer a malformed message by closing the connection).
 * @param {Command} c
 * @returns {Buffer}
 */
export function encodeCommand(c) {
  if (c.kind === 'new') {
    if (c.side !== Side.BUY && c.side !== Side.SELL) {
      throw new RangeError(`invalid side ${c.side}`);
    }
    if (c.orderType !== OrderType.LIMIT && c.orderType !== OrderType.MARKET) {
      throw new RangeError(`invalid order type ${c.orderType}`);
    }
    const b = Buffer.alloc(LEN_BYTES + 1 + NEW_ORDER_BODY);
    b.writeUInt32LE(1 + NEW_ORDER_BODY, 0);
    b.writeUInt8(MsgType.NEW_ORDER, 4);
    b.writeUInt8(c.side, 5);
    b.writeUInt8(c.orderType, 6);
    b.writeUInt16LE(checkUint(c.symbol, 0xffff, 'symbol'), 7);
    b.writeUInt32LE(checkUint(c.user, 0xffffffff, 'user'), 9);
    writeI64(b, c.price, 13, 'price');
    writeI64(b, c.qty, 21, 'qty');
    b.writeBigUInt64LE(checkBigint(c.clientRef, 'clientRef'), 29);
    return b;
  }
  if (c.kind === 'queryAccount') {
    const b = Buffer.alloc(LEN_BYTES + 1 + QUERY_ACCOUNT_BODY);
    b.writeUInt32LE(1 + QUERY_ACCOUNT_BODY, 0);
    b.writeUInt8(MsgType.QUERY_ACCOUNT, 4);
    b.writeUInt32LE(checkUint(c.user, 0xffffffff, 'user'), 5);
    b.writeBigUInt64LE(checkBigint(c.clientRef, 'clientRef'), 9);
    return b;
  }
  if (c.kind === 'queryInfo') {
    const b = Buffer.alloc(LEN_BYTES + 1 + QUERY_INFO_BODY);
    b.writeUInt32LE(1 + QUERY_INFO_BODY, 0);
    b.writeUInt8(MsgType.QUERY_INFO, 4);
    b.writeBigUInt64LE(checkBigint(c.clientRef, 'clientRef'), 5);
    return b;
  }
  if (c.kind === 'queryBook') {
    const b = Buffer.alloc(LEN_BYTES + 1 + QUERY_BOOK_BODY);
    b.writeUInt32LE(1 + QUERY_BOOK_BODY, 0);
    b.writeUInt8(MsgType.QUERY_BOOK, 4);
    b.writeUInt16LE(checkUint(c.symbol, 0xffff, 'symbol'), 5);
    b.writeUInt32LE(checkUint(c.depth, 0xffffffff, 'depth'), 7);
    b.writeBigUInt64LE(checkBigint(c.clientRef, 'clientRef'), 11);
    return b;
  }
  if (c.kind !== 'cancel') throw new TypeError(`unknown command kind ${c.kind}`);
  const b = Buffer.alloc(LEN_BYTES + 1 + CANCEL_BODY);
  b.writeUInt32LE(1 + CANCEL_BODY, 0);
  b.writeUInt8(MsgType.CANCEL_ORDER, 4);
  b.writeUInt16LE(checkUint(c.symbol, 0xffff, 'symbol'), 5);
  b.writeUInt32LE(checkUint(c.user, 0xffffffff, 'user'), 7);
  b.writeBigUInt64LE(checkBigint(c.orderId, 'orderId'), 11);
  b.writeBigUInt64LE(checkBigint(c.clientRef, 'clientRef'), 19);
  return b;
}

// ---- events (engine -> gateway) ----

function toSafe(v, what) {
  if (v > BigInt(Number.MAX_SAFE_INTEGER) || v < BigInt(Number.MIN_SAFE_INTEGER)) {
    throw new RangeError(`${what} does not fit in a JavaScript number: ${v}`);
  }
  return Number(v);
}

/**
 * Decodes the 108-byte body of an EVENT frame into:
 *   { type, reason, status, side, symbol, user, orderId, price, qty, remaining, clientRef,
 *     trade: { tradeId, symbol, price, qty, buyOrderId, sellOrderId, buyerId, sellerId, seq } }
 * `type` is an EventType, `reason` a RejectReason, `status` an OrderStatus, `side` a Side.
 * `trade` is meaningful only when type === EventType.TRADE.
 * Throws ProtocolError (wrong length or invalid enum) or RangeError (number too large).
 * @param {Buffer} body
 */
export function decodeEvent(body) {
  if (body.length !== EVENT_BODY) {
    throw new ProtocolError(`event body must be ${EVENT_BODY} bytes, got ${body.length}`);
  }
  const type = body.readUInt8(0);
  const reason = body.readUInt8(1);
  const status = body.readUInt8(2);
  const side = body.readUInt8(3);
  if (type > 10 || reason > 8 || status > 4 || side > 1) {
    throw new ProtocolError('event holds an invalid enum value');
  }
  return {
    type,
    reason,
    status,
    side,
    symbol: body.readUInt16LE(4),
    user: body.readUInt32LE(6),
    orderId: body.readBigUInt64LE(10),
    price: toSafe(body.readBigInt64LE(18), 'price'),
    qty: toSafe(body.readBigInt64LE(26), 'qty'),
    remaining: toSafe(body.readBigInt64LE(34), 'remaining'),
    clientRef: body.readBigUInt64LE(42),
    trade: {
      tradeId: body.readBigUInt64LE(50),
      symbol: body.readUInt16LE(58),
      price: toSafe(body.readBigInt64LE(60), 'trade price'),
      qty: toSafe(body.readBigInt64LE(68), 'trade qty'),
      buyOrderId: body.readBigUInt64LE(76),
      sellOrderId: body.readBigUInt64LE(84),
      buyerId: body.readUInt32LE(92),
      sellerId: body.readUInt32LE(96),
      seq: body.readBigUInt64LE(100),
    },
  };
}

// ---- turning a TCP byte stream into frames ----

/** @param {{type: number, body: Buffer}} frame */
export function decodeEventFrame(frame) {
  if (frame.type !== MsgType.EVENT) {
    throw new ProtocolError(`expected an EVENT frame, got type ${frame.type}`);
  }
  return decodeEvent(frame.body);
}

/**
 * TCP delivers a byte stream with no message boundaries: one data chunk can hold half a
 * frame or several. Feed every chunk to push(); it returns the frames now complete, each
 * as { type, body }. After a ProtocolError the stream is broken: close the connection.
 */
export class FrameReader {
  #pending = Buffer.alloc(0);
  #failed = false;

  /** @param {Buffer} chunk @returns {{type: number, body: Buffer}[]} */
  push(chunk) {
    if (this.#failed) throw new ProtocolError('the stream already failed');
    const data = this.#pending.length === 0 ? chunk : Buffer.concat([this.#pending, chunk]);
    const frames = [];
    let pos = 0;
    while (data.length - pos >= LEN_BYTES) {
      const len = data.readUInt32LE(pos);
      if (len < 1 || len > MAX_FRAME_LENGTH) {
        this.#failed = true;
        throw new ProtocolError(`invalid frame length ${len}`);
      }
      if (data.length - pos < LEN_BYTES + len) break;
      frames.push({
        type: data.readUInt8(pos + LEN_BYTES),
        body: data.subarray(pos + LEN_BYTES + 1, pos + LEN_BYTES + len),
      });
      pos += LEN_BYTES + len;
    }
    this.#pending = Buffer.from(data.subarray(pos)); // copy: do not keep the whole chunk alive
    return frames;
  }

  /** Bytes received but not yet part of a complete frame. */
  get buffered() {
    return this.#pending.length;
  }
}
// ---- decoding commands (used by the test fake engine; the real engine does this in C++) ----

/**
 * Decodes a NEW_ORDER or CANCEL_ORDER frame into the same object encodeCommand accepts.
 * @param {{type: number, body: Buffer}} frame
 */
export function decodeCommandFrame(frame) {
  const b = frame.body;
  if (frame.type === MsgType.NEW_ORDER) {
    if (b.length !== NEW_ORDER_BODY) throw new ProtocolError(`NEW_ORDER body must be ${NEW_ORDER_BODY} bytes, got ${b.length}`);
    const side = b.readUInt8(0);
    const orderType = b.readUInt8(1);
    if (side > 1 || orderType > 1) throw new ProtocolError('NEW_ORDER holds an invalid enum value');
    return {
      kind: 'new',
      side,
      orderType,
      symbol: b.readUInt16LE(2),
      user: b.readUInt32LE(4),
      price: toSafe(b.readBigInt64LE(8), 'price'),
      qty: toSafe(b.readBigInt64LE(16), 'qty'),
      clientRef: b.readBigUInt64LE(24),
    };
  }
  if (frame.type === MsgType.CANCEL_ORDER) {
    if (b.length !== CANCEL_BODY) throw new ProtocolError(`CANCEL_ORDER body must be ${CANCEL_BODY} bytes, got ${b.length}`);
    return {
      kind: 'cancel',
      symbol: b.readUInt16LE(0),
      user: b.readUInt32LE(2),
      orderId: b.readBigUInt64LE(6),
      clientRef: b.readBigUInt64LE(14),
    };
  }
  if (frame.type === MsgType.QUERY_ACCOUNT) {
    if (b.length !== QUERY_ACCOUNT_BODY) throw new ProtocolError(`QUERY_ACCOUNT body must be ${QUERY_ACCOUNT_BODY} bytes, got ${b.length}`);
    return { kind: 'queryAccount', user: b.readUInt32LE(0), clientRef: b.readBigUInt64LE(4) };
  }
  if (frame.type === MsgType.QUERY_BOOK) {
    if (b.length !== QUERY_BOOK_BODY) throw new ProtocolError(`QUERY_BOOK body must be ${QUERY_BOOK_BODY} bytes, got ${b.length}`);
    return { kind: 'queryBook', symbol: b.readUInt16LE(0), depth: b.readUInt32LE(2), clientRef: b.readBigUInt64LE(6) };
  }
  if (frame.type === MsgType.QUERY_INFO) {
    if (b.length !== QUERY_INFO_BODY) throw new ProtocolError(`QUERY_INFO body must be ${QUERY_INFO_BODY} bytes, got ${b.length}`);
    return { kind: 'queryInfo', clientRef: b.readBigUInt64LE(0) };
  }
  throw new ProtocolError(`unknown command type ${frame.type}`);
}
