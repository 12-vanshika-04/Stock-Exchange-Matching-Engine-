import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  EventType,
  FrameReader,
  ProtocolError,
  decodeCommandFrame,
  decodeEvent,
  encodeCommand,
} from '../src/protocol.js';
import { eventFrame } from './helpers.js';

const queryAccount = { kind: 'queryAccount', user: 7, clientRef: 0x11n };
const queryBook = { kind: 'queryBook', symbol: 3, depth: 20, clientRef: 0x22n };

// The same vectors as the C++ tests (engine/tests/protocol_test.cpp).
test('QUERY_ACCOUNT frame has the bytes the C++ test expects', () => {
  assert.deepEqual([...encodeCommand(queryAccount)], [
    0x0d, 0x00, 0x00, 0x00, // length = 13
    0x03, // QUERY_ACCOUNT
    0x07, 0x00, 0x00, 0x00, // user 7
    0x11, 0, 0, 0, 0, 0, 0, 0, // clientRef
  ]);
});

test('QUERY_BOOK frame has the bytes the C++ test expects', () => {
  assert.deepEqual([...encodeCommand(queryBook)], [
    0x0f, 0x00, 0x00, 0x00, // length = 15
    0x04, // QUERY_BOOK
    0x03, 0x00, // symbol 3
    0x14, 0x00, 0x00, 0x00, // depth 20
    0x22, 0, 0, 0, 0, 0, 0, 0, // clientRef
  ]);
});

test('queries survive encode, frame reader and decode unchanged, and bad values are refused', () => {
  for (const c of [queryAccount, queryBook]) {
    const [frame] = new FrameReader().push(encodeCommand(c));
    assert.deepEqual(decodeCommandFrame(frame), c);
    assert.throws(() => decodeCommandFrame({ type: frame.type, body: frame.body.subarray(1) }), ProtocolError);
  }
  assert.throws(() => encodeCommand({ ...queryBook, depth: -1 }), RangeError);
  assert.throws(() => encodeCommand({ ...queryBook, symbol: 70000 }), RangeError);
  assert.throws(() => encodeCommand({ ...queryAccount, clientRef: 5 }), TypeError);
});

test('the query event types decode, and the next value does not', () => {
  const types = [
    EventType.ACCOUNT_BALANCE,
    EventType.ACCOUNT_POSITION,
    EventType.BOOK_LEVEL,
    EventType.QUERY_DONE,
    EventType.QUERY_REJECTED,
  ];
  for (const type of types) {
    const e = decodeEvent(eventFrame({ type, symbol: 2, user: 9, price: -5, qty: 77, remaining: 3, clientRef: 42n }).subarray(5));
    assert.equal(e.type, type);
    assert.equal(e.symbol, 2);
    assert.equal(e.price, -5);
    assert.equal(e.qty, 77);
    assert.equal(e.remaining, 3);
    assert.equal(e.clientRef, 42n);
  }
  assert.throws(() => decodeEvent(eventFrame({ type: 11 }).subarray(5)), ProtocolError);
});