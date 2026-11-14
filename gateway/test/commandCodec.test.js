import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  FrameReader, MsgType, OrderType, ProtocolError, Side,
  decodeCommandFrame, encodeCommand, makeOrderId,
} from '../src/protocol.js';

const newOrder = {
  kind: 'new', side: Side.SELL, orderType: OrderType.LIMIT,
  symbol: 5, user: 0x01020304, price: 50000, qty: 100, clientRef: 0x0102030405060708n,
};
const cancel = { kind: 'cancel', symbol: 3, user: 7, orderId: makeOrderId(3, 9), clientRef: 0x11n };

test('commands survive encode, frame reader and decode unchanged', () => {
  const market = { ...newOrder, side: Side.BUY, orderType: OrderType.MARKET, price: 0, qty: 7, clientRef: 2n ** 64n - 1n };
  for (const c of [newOrder, cancel, market]) {
    const frames = new FrameReader().push(encodeCommand(c));
    assert.equal(frames.length, 1);
    assert.deepEqual(decodeCommandFrame(frames[0]), c);
  }
});

test('decodeCommandFrame refuses malformed frames', () => {
  const [frame] = new FrameReader().push(encodeCommand(newOrder));
  assert.throws(() => decodeCommandFrame({ type: frame.type, body: frame.body.subarray(1) }), ProtocolError);
  const badSide = Buffer.from(frame.body);
  badSide[0] = 2;
  assert.throws(() => decodeCommandFrame({ type: frame.type, body: badSide }), ProtocolError);
  assert.throws(() => decodeCommandFrame({ type: MsgType.EVENT, body: frame.body }), ProtocolError);
});