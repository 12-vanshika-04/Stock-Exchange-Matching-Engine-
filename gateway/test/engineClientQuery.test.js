import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EngineClient } from '../src/engineClient.js';
import { EventType, RejectReason, Side } from '../src/protocol.js';
import { startFakeEngine } from './fakeEngine.js';
import { eventFrame } from './helpers.js';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function setup(onCommand) {
  const fake = await startFakeEngine(onCommand);
  const client = new EngineClient({ port: fake.port, minDelayMs: 10, maxDelayMs: 50 });
  client.on('problem', () => {});
  await client.start({ timeoutMs: 3000 });
  return {
    fake,
    client,
    async cleanup() {
      await client.stop();
      await fake.close();
    },
  };
}

test('a query resolves with the whole answer in order, without the end marker', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.ACCOUNT_BALANCE, user: cmd.user, price: 5000000, qty: 1250000, clientRef: cmd.clientRef }),
    eventFrame({ type: EventType.ACCOUNT_POSITION, user: cmd.user, symbol: 0, qty: 1000, remaining: 100, price: 250000000, clientRef: cmd.clientRef }),
    eventFrame({ type: EventType.QUERY_DONE, user: cmd.user, clientRef: cmd.clientRef }),
  ]);
  try {
    const seen = [];
    t.client.on('event', (e) => seen.push(e));
    const events = await t.client.query({ kind: 'queryAccount', user: 5 });
    assert.deepEqual(events.map((e) => e.type), [EventType.ACCOUNT_BALANCE, EventType.ACCOUNT_POSITION]);
    assert.equal(events[0].price, 5000000);
    assert.equal(events[1].remaining, 100);
    assert.equal(seen.length, 3); // listeners still see every event, the end marker included
  } finally {
    await t.cleanup();
  }
});

test('events of other requests and other queries do not leak into the answer', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.BOOK_LEVEL, symbol: 0, side: Side.BUY, price: 249995, qty: 25, clientRef: cmd.clientRef }),
    eventFrame({ type: EventType.ORDER_ACCEPTED, clientRef: cmd.clientRef + 1n }), // someone else's reply
    eventFrame({ type: EventType.BOOK_LEVEL, symbol: 0, side: Side.SELL, price: 250005, qty: 7, clientRef: cmd.clientRef + 1n }), // another query's level
    eventFrame({ type: EventType.BOOK_LEVEL, symbol: 0, side: Side.SELL, price: 250010, qty: 3, clientRef: cmd.clientRef }),
    eventFrame({ type: EventType.QUERY_DONE, symbol: 0, clientRef: cmd.clientRef }),
  ]);
  try {
    const events = await t.client.query({ kind: 'queryBook', symbol: 0, depth: 5 });
    assert.deepEqual(events.map((e) => [e.side, e.price, e.qty]), [
      [Side.BUY, 249995, 25],
      [Side.SELL, 250010, 3],
    ]);
  } finally {
    await t.cleanup();
  }
});

test('a refused query rejects with the engine reason', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.QUERY_REJECTED, reason: RejectReason.UNKNOWN_USER, user: cmd.user, clientRef: cmd.clientRef }),
  ]);
  try {
    await assert.rejects(
      t.client.query({ kind: 'queryAccount', user: 99999 }),
      (err) => err.code === 'QUERY_REJECTED' && err.reason === RejectReason.UNKNOWN_USER,
    );
  } finally {
    await t.cleanup();
  }
});

test('a query with no reply times out', async () => {
  const t = await setup(() => []);
  try {
    await assert.rejects(t.client.query({ kind: 'queryAccount', user: 5 }, { timeoutMs: 50 }), { code: 'ETIMEDOUT' });
  } finally {
    await t.cleanup();
  }
});

test('a connection that drops in the middle of an answer rejects the query', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.ACCOUNT_BALANCE, user: cmd.user, price: 1, qty: 0, clientRef: cmd.clientRef }), // never finishes
  ]);
  try {
    const outcome = assert.rejects(t.client.query({ kind: 'queryAccount', user: 5 }, { timeoutMs: 3000 }), {
      code: 'ECONNLOST',
    });
    await sleep(50);
    await t.fake.close();
    await outcome;
  } finally {
    await t.cleanup();
  }
});