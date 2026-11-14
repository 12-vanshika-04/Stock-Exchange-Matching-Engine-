import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  EVENT_BODY,
  EventType,
  FrameReader,
  MsgType,
  OrderStatus,
  OrderType,
  ProtocolError,
  RejectReason,
  Side,
  decodeEvent,
  decodeEventFrame,
  encodeCommand,
  localOrderId,
  makeOrderId,
  symbolOfOrderId,
} from '../src/protocol.js';

const newOrder = {
  kind: 'new',
  side: Side.SELL,
  orderType: OrderType.LIMIT,
  symbol: 5,
  user: 0x01020304,
  price: 50000,
  qty: 100,
  clientRef: 0x0102030405060708n,
};

const cancel = {
  kind: 'cancel',
  symbol: 3,
  user: 7,
  orderId: makeOrderId(3, 9),
  clientRef: 0x11n,
};

// Builds an event body from the documented layout. Any field can be overridden.
function eventBody(o = {}) {
  const b = Buffer.alloc(EVENT_BODY);
  b.writeUInt8(o.type ?? EventType.TRADE, 0);
  b.writeUInt8(o.reason ?? RejectReason.PRICE_OUT_OF_BAND, 1);
  b.writeUInt8(o.status ?? OrderStatus.PARTIAL, 2);
  b.writeUInt8(o.side ?? Side.SELL, 3);
  b.writeUInt16LE(4, 4);
  b.writeUInt32LE(123456, 6);
  b.writeBigUInt64LE(o.orderId ?? makeOrderId(4, 77), 10);
  b.writeBigInt64LE(o.price ?? -12345n, 18);
  b.writeBigInt64LE(9876543210n, 26);
  b.writeBigInt64LE(42n, 34);
  b.writeBigUInt64LE(0xdeadbeefcafen, 42);
  b.writeBigUInt64LE(99n, 50);
  b.writeUInt16LE(4, 58);
  b.writeBigInt64LE(50500n, 60);
  b.writeBigInt64LE(17n, 68);
  b.writeBigUInt64LE(makeOrderId(4, 5), 76);
  b.writeBigUInt64LE(makeOrderId(4, 6), 84);
  b.writeUInt32LE(11, 92);
  b.writeUInt32LE(22, 96);
  b.writeBigUInt64LE(1234n, 100);
  return b;
}

function wrap(type, body) {
  const head = Buffer.alloc(5);
  head.writeUInt32LE(1 + body.length, 0);
  head.writeUInt8(type, 4);
  return Buffer.concat([head, body]);
}

// ---- the exact bytes: the same vectors as the C++ tests (engine/tests/protocol_test.cpp) ----

test('NEW_ORDER frame has the bytes the C++ test expects', () => {
  assert.deepEqual([...encodeCommand(newOrder)], [
    0x21, 0x00, 0x00, 0x00, // length = 33
    0x01, // NEW_ORDER
    0x01, 0x00, // side SELL, order type LIMIT
    0x05, 0x00, // symbol 5
    0x04, 0x03, 0x02, 0x01, // user 0x01020304
    0x50, 0xc3, 0, 0, 0, 0, 0, 0, // price 50000
    0x64, 0, 0, 0, 0, 0, 0, 0, // qty 100
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // clientRef
  ]);
});

test('CANCEL frame has the bytes the C++ test expects', () => {
  assert.deepEqual([...encodeCommand(cancel)], [
    0x17, 0x00, 0x00, 0x00, // length = 23
    0x02, // CANCEL_ORDER
    0x03, 0x00, // symbol 3
    0x07, 0x00, 0x00, 0x00, // user 7
    0x09, 0, 0, 0, 0, 0, 0x03, 0x00, // order id: local 9, symbol 3 in the top 16 bits
    0x11, 0, 0, 0, 0, 0, 0, 0, // clientRef 0x11
  ]);
});

test('encodeCommand rejects values the engine would refuse', () => {
  assert.throws(() => encodeCommand({ ...newOrder, user: 2 ** 32 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, user: -1 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, symbol: 70000 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, price: 1.5 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, qty: 2 ** 60 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, side: 2 }), RangeError);
  assert.throws(() => encodeCommand({ ...newOrder, orderType: 9 }), RangeError);
  // a number where a bigint belongs: the classic JavaScript mistake
  assert.throws(() => encodeCommand({ ...newOrder, clientRef: 5 }), TypeError);
  assert.throws(() => encodeCommand({ ...cancel, orderId: 9 }), TypeError);
  assert.throws(() => encodeCommand({ kind: 'bogus' }), TypeError);
});

test('order ids carry the symbol in the top 16 bits', () => {
  assert.equal(makeOrderId(3, 9), (3n << 48n) | 9n);
  const big = makeOrderId(40, 5); // beyond 2^53: only a bigint holds it exactly
  assert.ok(big > BigInt(Number.MAX_SAFE_INTEGER));
  assert.equal(symbolOfOrderId(big), 40);
  assert.equal(localOrderId(big), 5n);
});

// ---- frames out of a byte stream ----

test('frames split across chunks are reassembled', () => {
  const stream = Buffer.concat([
    encodeCommand(newOrder),
    encodeCommand(cancel),
    wrap(MsgType.EVENT, eventBody()),
  ]);
  const reader = new FrameReader();
  const types = [];
  for (const byte of stream) {
    for (const f of reader.push(Buffer.from([byte]))) types.push(f.type); // one byte per "read"
  }
  assert.deepEqual(types, [1, 2, 16]);
  assert.equal(reader.buffered, 0);
});

test('several frames in one chunk are all delivered', () => {
  const parts = [];
  for (let i = 0; i < 50; i++) parts.push(encodeCommand(newOrder), encodeCommand(cancel));
  const frames = new FrameReader().push(Buffer.concat(parts));
  assert.equal(frames.length, 100);
  assert.equal(frames.filter((f) => f.type === MsgType.NEW_ORDER).length, 50);
  assert.equal(frames.filter((f) => f.type === MsgType.CANCEL_ORDER).length, 50);
});

test('a zero or oversized length is a protocol error, and the reader stays failed', () => {
  assert.throws(() => new FrameReader().push(Buffer.from([0, 0, 0, 0])), ProtocolError);
  const reader = new FrameReader();
  assert.throws(() => reader.push(Buffer.from([0x01, 0x04, 0, 0])), ProtocolError); // 1025
  assert.throws(() => reader.push(encodeCommand(newOrder)), ProtocolError);
});

// ---- events ----

test('decodeEvent reads every field', () => {
  assert.deepEqual(decodeEvent(eventBody()), {
    type: EventType.TRADE,
    reason: RejectReason.PRICE_OUT_OF_BAND,
    status: OrderStatus.PARTIAL,
    side: Side.SELL,
    symbol: 4,
    user: 123456,
    orderId: makeOrderId(4, 77),
    price: -12345,
    qty: 9876543210,
    remaining: 42,
    clientRef: 0xdeadbeefcafen,
    trade: {
      tradeId: 99n,
      symbol: 4,
      price: 50500,
      qty: 17,
      buyOrderId: makeOrderId(4, 5),
      sellOrderId: makeOrderId(4, 6),
      buyerId: 11,
      sellerId: 22,
      seq: 1234n,
    },
  });
});

test('order ids beyond 2^53 survive decoding exactly', () => {
  const id = makeOrderId(40, 5);
  assert.equal(decodeEvent(eventBody({ orderId: id })).orderId, id);
});

test('decodeEvent rejects a wrong length and invalid enum values', () => {
  const good = eventBody();
  assert.throws(() => decodeEvent(good.subarray(0, EVENT_BODY - 1)), ProtocolError);
  assert.throws(() => decodeEvent(Buffer.concat([good, Buffer.alloc(1)])), ProtocolError);
  assert.throws(() => decodeEvent(eventBody({ type: 10 })), ProtocolError);
  assert.throws(() => decodeEvent(eventBody({ reason: 9 })), ProtocolError);
  assert.throws(() => decodeEvent(eventBody({ status: 9 })), ProtocolError);
  assert.throws(() => decodeEvent(eventBody({ side: 9 })), ProtocolError);
});

test('decodeEvent refuses numbers JavaScript cannot hold exactly', () => {
  assert.throws(() => decodeEvent(eventBody({ price: 2n ** 60n })), RangeError);
});

test('decodeEventFrame refuses frames that are not events', () => {
  assert.throws(() => decodeEventFrame({ type: MsgType.NEW_ORDER, body: eventBody() }), ProtocolError);
  assert.equal(decodeEventFrame({ type: MsgType.EVENT, body: eventBody() }).symbol, 4);
});