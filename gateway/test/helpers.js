import { EVENT_BODY, EventType, MsgType, OrderStatus, RejectReason, Side } from '../src/protocol.js';

/**
 * Builds a complete EVENT frame (113 bytes) from the documented layout.
 * Any field can be overridden; `trade` takes { tradeId, symbol, price, qty, buyOrderId,
 * sellOrderId, buyerId, sellerId, seq }.
 */
export function eventFrame(o = {}) {
  const body = Buffer.alloc(EVENT_BODY);
  body.writeUInt8(o.type ?? EventType.ORDER_ACCEPTED, 0);
  body.writeUInt8(o.reason ?? RejectReason.NONE, 1);
  body.writeUInt8(o.status ?? OrderStatus.NEW, 2);
  body.writeUInt8(o.side ?? Side.BUY, 3);
  body.writeUInt16LE(o.symbol ?? 0, 4);
  body.writeUInt32LE(o.user ?? 0, 6);
  body.writeBigUInt64LE(o.orderId ?? 0n, 10);
  body.writeBigInt64LE(BigInt(o.price ?? 0), 18);
  body.writeBigInt64LE(BigInt(o.qty ?? 0), 26);
  body.writeBigInt64LE(BigInt(o.remaining ?? 0), 34);
  body.writeBigUInt64LE(o.clientRef ?? 0n, 42);
  const t = o.trade ?? {};
  body.writeBigUInt64LE(t.tradeId ?? 0n, 50);
  body.writeUInt16LE(t.symbol ?? 0, 58);
  body.writeBigInt64LE(BigInt(t.price ?? 0), 60);
  body.writeBigInt64LE(BigInt(t.qty ?? 0), 68);
  body.writeBigUInt64LE(t.buyOrderId ?? 0n, 76);
  body.writeBigUInt64LE(t.sellOrderId ?? 0n, 84);
  body.writeUInt32LE(t.buyerId ?? 0, 92);
  body.writeUInt32LE(t.sellerId ?? 0, 96);
  body.writeBigUInt64LE(t.seq ?? 0n, 100);
  const head = Buffer.alloc(5);
  head.writeUInt32LE(1 + EVENT_BODY, 0);
  head.writeUInt8(MsgType.EVENT, 4);
  return Buffer.concat([head, body]);
}