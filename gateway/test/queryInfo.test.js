import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EngineClient } from '../src/engineClient.js';
import {
  EventType,
  FrameReader,
  ProtocolError,
  decodeCommandFrame,
  decodeEvent,
  encodeCommand,
} from '../src/protocol.js';
import { startFakeEngine } from './fakeEngine.js';
import { eventFrame } from './helpers.js';

const queryInfo = { kind: 'queryInfo', clientRef: 0x33n };

// The same vector as the C++ test (engine/tests/protocol_test.cpp).
test('QUERY_INFO frame has the bytes the C++ test expects', () => {
  assert.deepEqual([...encodeCommand(queryInfo)], [
    0x09, 0x00, 0x00, 0x00, // length = 9
    0x05, // QUERY_INFO
    0x33, 0, 0, 0, 0, 0, 0, 0, // clientRef
  ]);
});

test('QUERY_INFO survives decoding, and a wrong length or a number as clientRef is refused', () => {
  const [frame] = new FrameReader().push(encodeCommand(queryInfo));
  assert.deepEqual(decodeCommandFrame(frame), queryInfo);
  assert.throws(() => decodeCommandFrame({ type: frame.type, body: frame.body.subarray(1) }), ProtocolError);
  assert.throws(() => encodeCommand({ kind: 'queryInfo', clientRef: 5 }), TypeError);
});

test('ENGINE_INFO decodes, including the largest engine id (2^52 - 1)', () => {
  const e = decodeEvent(
    eventFrame({ type: EventType.ENGINE_INFO, qty: 4503599627370495, remaining: 6, clientRef: 7n }).subarray(5),
  );
  assert.equal(e.type, EventType.ENGINE_INFO);
  assert.equal(e.qty, 4503599627370495);
  assert.equal(e.remaining, 6);
  assert.equal(e.clientRef, 7n);
});

test('engineId() asks the engine and returns the id as a string', async () => {
  const fake = await startFakeEngine((cmd) => [
    eventFrame({ type: EventType.ENGINE_INFO, qty: 4503599627370495, remaining: 6, clientRef: cmd.clientRef }),
    eventFrame({ type: EventType.QUERY_DONE, clientRef: cmd.clientRef }),
  ]);
  const client = new EngineClient({ port: fake.port, minDelayMs: 10, maxDelayMs: 50 });
  client.on('problem', () => {});
  try {
    await client.start({ timeoutMs: 3000 });
    assert.deepEqual(await client.engineId(), { engineId: '4503599627370495', symbols: 6 });
    assert.equal(fake.commands[0].kind, 'queryInfo');
  } finally {
    await client.stop();
    await fake.close();
  }
});
