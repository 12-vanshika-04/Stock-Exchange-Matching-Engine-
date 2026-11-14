import { test } from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createApp } from '../src/app.js';
import { EngineClient } from '../src/engineClient.js';
import { EventType, RejectReason, Side } from '../src/protocol.js';
import { startFakeEngine } from './fakeEngine.js';
import { eventFrame } from './helpers.js';

async function setup(onCommand) {
  const engine = await startFakeEngine(onCommand);
  const client = new EngineClient({ port: engine.port, minDelayMs: 10, maxDelayMs: 50 });
  client.on('problem', () => {});
  await client.start({ timeoutMs: 3000 });
  const server = createApp({ engine: client }).listen(0, '127.0.0.1');
  await once(server, 'listening');
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    engine,
    async cleanup() {
      server.closeAllConnections();
      await new Promise((resolve) => server.close(resolve));
      await client.stop();
      await engine.close();
    },
  };
}

async function get(base, path, user = '7') {
  const headers = user === null ? {} : { 'x-user-id': user };
  const res = await fetch(base + path, { headers });
  return { status: res.status, json: await res.json() };
}

test('GET /api/account returns cash and one position per symbol, in rupees', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.ACCOUNT_BALANCE, user: cmd.user, price: 5000000, qty: 1250000, clientRef: cmd.clientRef }),
    ...[0, 1, 2, 3, 4, 5].map((symbol) =>
      eventFrame({
        type: EventType.ACCOUNT_POSITION,
        user: cmd.user,
        symbol,
        qty: symbol === 0 ? 1000 : 0,
        remaining: symbol === 0 ? 100 : 0,
        price: symbol === 0 ? 250000000 : 0,
        clientRef: cmd.clientRef,
      }),
    ),
    eventFrame({ type: EventType.QUERY_DONE, user: cmd.user, clientRef: cmd.clientRef }),
  ]);
  try {
    const r = await get(t.base, '/api/account', '7');
    assert.equal(r.status, 200);
    assert.equal(r.json.cash, '50000.00');
    assert.equal(r.json.reservedCash, '12500.00');
    assert.equal(r.json.availableCash, '37500.00');
    assert.equal(r.json.positions.length, 6);
    assert.deepEqual(r.json.positions[0], {
      symbol: 'RELIANCE', quantity: 1000, reserved: 100, available: 900, averageCost: '2500.00', costBasis: '2500000.00',
    });
    assert.deepEqual(r.json.positions[1], {
      symbol: 'TCS', quantity: 0, reserved: 0, available: 0, averageCost: '0.00', costBasis: '0.00',
    });
    assert.equal(t.engine.commands[0].kind, 'queryAccount');
    assert.equal(t.engine.commands[0].user, 7);
  } finally {
    await t.cleanup();
  }
});

test('GET /api/account needs a login, and an unknown account is refused', async () => {
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.QUERY_REJECTED, reason: RejectReason.UNKNOWN_USER, user: cmd.user, clientRef: cmd.clientRef }),
  ]);
  try {
    const noLogin = await get(t.base, '/api/account', null);
    assert.equal(noLogin.status, 401);
    assert.equal(t.engine.commands.length, 0);
    const unknown = await get(t.base, '/api/account', '99999');
    assert.equal(unknown.status, 403);
    assert.equal(unknown.json.error, 'UNKNOWN_USER');
  } finally {
    await t.cleanup();
  }
});

test('GET /api/book/:symbol returns bids and asks best first, and defaults the depth to 10', async () => {
  const level = (cmd, side, price, qty) =>
    eventFrame({ type: EventType.BOOK_LEVEL, symbol: cmd.symbol, side, price, qty, clientRef: cmd.clientRef });
  const t = await setup((cmd) => [
    level(cmd, Side.BUY, 249995, 25),
    level(cmd, Side.BUY, 249990, 10),
    level(cmd, Side.SELL, 250005, 7),
    eventFrame({ type: EventType.QUERY_DONE, symbol: cmd.symbol, clientRef: cmd.clientRef }),
  ]);
  try {
    const r = await get(t.base, '/api/book/reliance?depth=2', null); // public: no login, any letter case
    assert.equal(r.status, 200);
    assert.deepEqual(r.json, {
      symbol: 'RELIANCE',
      bids: [{ price: '2499.95', quantity: 25 }, { price: '2499.90', quantity: 10 }],
      asks: [{ price: '2500.05', quantity: 7 }],
    });
    assert.equal(t.engine.commands[0].kind, 'queryBook');
    assert.equal(t.engine.commands[0].symbol, 0);
    assert.equal(t.engine.commands[0].depth, 2);

    await get(t.base, '/api/book/TCS', null);
    assert.equal(t.engine.commands[1].symbol, 1);
    assert.equal(t.engine.commands[1].depth, 10);
  } finally {
    await t.cleanup();
  }
});

test('GET /api/book/:symbol refuses bad symbols and depths without asking the engine', async () => {
  const t = await setup(() => []);
  try {
    assert.equal((await get(t.base, '/api/book/NOPE', null)).status, 404);
    for (const depth of ['0', 'abc', '51', '1.5', '-1', '']) {
      const r = await get(t.base, `/api/book/TCS?depth=${depth}`, null);
      assert.equal(r.status, 400, `depth=${depth}`);
      assert.equal(r.json.error, 'INVALID_DEPTH');
    }
    assert.equal(t.engine.commands.length, 0);
  } finally {
    await t.cleanup();
  }
});