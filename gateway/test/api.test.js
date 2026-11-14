import { test } from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createApp } from '../src/app.js';
import { EngineClient } from '../src/engineClient.js';
import { EventType, OrderStatus, OrderType, RejectReason, Side, makeOrderId } from '../src/protocol.js';
import { startFakeEngine } from './fakeEngine.js';
import { eventFrame } from './helpers.js';

async function serve(app) {
  const server = app.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    async close() {
      server.closeAllConnections();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}

// An API wired to a started EngineClient and a scripted fake engine.
async function setup(onCommand, options = {}) {
  const engine = await startFakeEngine(onCommand);
  const client = new EngineClient({ port: engine.port, minDelayMs: 10, maxDelayMs: 50 });
  client.on('problem', () => {});
  await client.start({ timeoutMs: 3000 });
  const web = await serve(createApp({ engine: client, ...options }));
  return {
    base: web.base,
    engine,
    async cleanup() {
      await web.close();
      await client.stop();
      await engine.close();
    },
  };
}

async function call(base, method, path, { body, user = '7', raw } = {}) {
  const headers = { 'content-type': 'application/json' };
  if (user !== null) headers['x-user-id'] = user;
  const res = await fetch(base + path, {
    method,
    headers,
    body: raw ?? (body === undefined ? undefined : JSON.stringify(body)),
  });
  return { status: res.status, json: await res.json() };
}

const goodOrder = { symbol: 'INFY', side: 'buy', type: 'limit', price: '500.05', quantity: 10 };

const accepted = (cmd, overrides = {}) =>
  eventFrame({
    type: EventType.ORDER_ACCEPTED,
    status: OrderStatus.NEW,
    side: cmd.side,
    symbol: cmd.symbol,
    user: cmd.user,
    orderId: makeOrderId(cmd.symbol, 5),
    price: cmd.price,
    qty: cmd.qty,
    remaining: cmd.qty,
    clientRef: cmd.clientRef,
    ...overrides,
  });

test('health reports the engine connection', async () => {
  const t = await setup();
  try {
    const r = await call(t.base, 'GET', '/api/health', { user: null });
    assert.equal(r.status, 200);
    assert.deepEqual(r.json, { ok: true, engineConnected: true });
  } finally {
    await t.cleanup();
  }
});

test('orders need the development login header', async () => {
  const t = await setup();
  try {
    const r = await call(t.base, 'POST', '/api/orders', { body: goodOrder, user: null });
    assert.equal(r.status, 401);
    assert.equal(r.json.error, 'UNAUTHENTICATED');
    assert.equal(t.engine.commands.length, 0);
  } finally {
    await t.cleanup();
  }
});

test('a limit order reaches the engine in the right form and the acknowledgement comes back', async () => {
  const t = await setup((cmd) => [accepted(cmd)]);
  try {
    const r = await call(t.base, 'POST', '/api/orders', { body: goodOrder });
    assert.equal(r.status, 201);
    assert.deepEqual(r.json, {
      orderId: String(makeOrderId(2, 5)),
      symbol: 'INFY',
      side: 'buy',
      price: '500.05',
      quantity: 10,
      remaining: 10,
      status: 'NEW',
    });
    const c = t.engine.commands[0];
    assert.equal(c.kind, 'new');
    assert.equal(c.side, Side.BUY);
    assert.equal(c.orderType, OrderType.LIMIT);
    assert.equal(c.symbol, 2); // INFY
    assert.equal(c.user, 7);
    assert.equal(c.price, 50005);
    assert.equal(c.qty, 10);
    assert.equal(typeof c.clientRef, 'bigint');
  } finally {
    await t.cleanup();
  }
});

test('a market order carries price 0 whatever the body says', async () => {
  const t = await setup((cmd) => [accepted(cmd, { status: OrderStatus.FILLED, remaining: 0 })]);
  try {
    const r = await call(t.base, 'POST', '/api/orders', {
      body: { symbol: 'sbin', side: 'sell', type: 'market', price: '999', quantity: 3 },
    });
    assert.equal(r.status, 201);
    assert.equal(r.json.status, 'FILLED');
    const c = t.engine.commands[0];
    assert.equal(c.orderType, OrderType.MARKET);
    assert.equal(c.side, Side.SELL);
    assert.equal(c.symbol, 5); // SBIN, the name is case-insensitive
    assert.equal(c.price, 0);
  } finally {
    await t.cleanup();
  }
});

test('malformed orders are refused before they reach the engine', async () => {
  const t = await setup((cmd) => [accepted(cmd)]);
  try {
    const bad = [
      { ...goodOrder, symbol: 'NOPE' },
      { ...goodOrder, symbol: 3 },
      { ...goodOrder, symbol: 'constructor' },
      { ...goodOrder, side: 'hold' },
      { ...goodOrder, side: 'constructor' },
      { ...goodOrder, type: 'stop' },
      { ...goodOrder, quantity: 0 },
      { ...goodOrder, quantity: 1.5 },
      { ...goodOrder, quantity: '5' },
      { ...goodOrder, quantity: 1e9 },
      { ...goodOrder, price: '500.123' },
      { ...goodOrder, price: 'abc' },
      { ...goodOrder, price: '-5' },
      { ...goodOrder, price: '0' },
      { ...goodOrder, price: 0 },
      { ...goodOrder, price: undefined }, // JSON drops it: a limit order with no price
    ];
    for (const body of bad) {
      const r = await call(t.base, 'POST', '/api/orders', { body });
      assert.equal(r.status, 400, JSON.stringify(body));
      assert.equal(typeof r.json.error, 'string');
    }
    const notAnObject = await call(t.base, 'POST', '/api/orders', { raw: '[1]' });
    assert.equal(notAnObject.status, 400);
    assert.equal(t.engine.commands.length, 0);
  } finally {
    await t.cleanup();
  }
});

test('engine rejections become HTTP errors with the reason', async () => {
  let reason = RejectReason.INSUFFICIENT_FUNDS;
  const t = await setup((cmd) => [
    eventFrame({ type: EventType.ORDER_REJECTED, status: OrderStatus.REJECTED, reason, symbol: cmd.symbol, user: cmd.user, clientRef: cmd.clientRef }),
  ]);
  try {
    const funds = await call(t.base, 'POST', '/api/orders', { body: goodOrder });
    assert.equal(funds.status, 422);
    assert.equal(funds.json.error, 'INSUFFICIENT_FUNDS');
    reason = RejectReason.UNKNOWN_USER;
    const unknown = await call(t.base, 'POST', '/api/orders', { body: goodOrder });
    assert.equal(unknown.status, 403);
    assert.equal(unknown.json.error, 'UNKNOWN_USER');
  } finally {
    await t.cleanup();
  }
});

test('cancel: success, not found, and bad ids', async () => {
  let type = EventType.ORDER_CANCELLED;
  const t = await setup((cmd) => [
    eventFrame({ type, status: OrderStatus.CANCELLED, symbol: cmd.symbol, user: cmd.user, orderId: cmd.orderId, clientRef: cmd.clientRef }),
  ]);
  try {
    const id = makeOrderId(2, 5);
    const ok = await call(t.base, 'DELETE', `/api/orders/${id}`);
    assert.equal(ok.status, 200);
    assert.deepEqual(ok.json, { orderId: String(id), status: 'CANCELLED' });
    const c = t.engine.commands[0];
    assert.equal(c.kind, 'cancel');
    assert.equal(c.symbol, 2); // taken from the order id
    assert.equal(c.user, 7);
    assert.equal(c.orderId, id);

    type = EventType.CANCEL_REJECTED;
    const missing = await call(t.base, 'DELETE', `/api/orders/${id}`);
    assert.equal(missing.status, 404);
    assert.equal(missing.json.error, 'ORDER_NOT_FOUND');

    for (const badId of ['abc', '99999999999999999999999', '18446744073709551616']) {
      const r = await call(t.base, 'DELETE', `/api/orders/${badId}`);
      assert.equal(r.status, 400, badId);
    }
    assert.equal(t.engine.commands.length, 2); // only the two valid ones got through
  } finally {
    await t.cleanup();
  }
});

test('a silent engine gives 504, a missing engine gives 503', async () => {
  const silent = await setup(() => [], { requestTimeoutMs: 100 });
  try {
    const r = await call(silent.base, 'POST', '/api/orders', { body: goodOrder });
    assert.equal(r.status, 504);
    assert.equal(r.json.error, 'ENGINE_TIMEOUT');
  } finally {
    await silent.cleanup();
  }

  const web = await serve(createApp({ engine: new EngineClient({ port: 1 }) })); // never started
  try {
    const r = await call(web.base, 'POST', '/api/orders', { body: goodOrder });
    assert.equal(r.status, 503);
    assert.equal(r.json.error, 'ENGINE_UNAVAILABLE');
    const health = await call(web.base, 'GET', '/api/health', { user: null });
    assert.deepEqual(health.json, { ok: true, engineConnected: false });
  } finally {
    await web.close();
  }
});

test('invalid JSON gets a clear 400', async () => {
  const t = await setup();
  try {
    const r = await call(t.base, 'POST', '/api/orders', { raw: '{bad' });
    assert.equal(r.status, 400);
    assert.equal(r.json.error, 'INVALID_JSON');
  } finally {
    await t.cleanup();
  }
});