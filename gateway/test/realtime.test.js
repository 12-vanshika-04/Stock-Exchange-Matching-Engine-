import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { once } from 'node:events';
import { io as connect } from 'socket.io-client';
import { createApp } from '../src/app.js';
import { attachRealtime } from '../src/realtime.js';
import { EngineClient } from '../src/engineClient.js';
import { EventType, OrderStatus, Side, makeOrderId } from '../src/protocol.js';
import { startFakeEngine } from './fakeEngine.js';
import { eventFrame } from './helpers.js';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(condition, ms = 3000) {
  const end = Date.now() + ms;
  while (!condition()) {
    if (Date.now() > end) throw new Error('timed out waiting for a condition');
    await sleep(5);
  }
}

function waitEvent(socket, name, ms = 3000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timed out waiting for "${name}"`)), ms);
    socket.once(name, (...args) => {
      clearTimeout(timer);
      resolve(args.length > 1 ? args : args[0]);
    });
  });
}

// emit with an acknowledgement callback, as a promise
function rpc(socket, event, payload, ms = 3000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`no acknowledgement for "${event}"`)), ms);
    socket.emit(event, payload, (reply) => {
      clearTimeout(timer);
      resolve(reply);
    });
  });
}

// The real app and realtime layer, wired to a fake engine we can push events from.
async function setup(onCommand) {
  const fake = await startFakeEngine(onCommand);
  const engine = new EngineClient({ port: fake.port, minDelayMs: 10, maxDelayMs: 50 });
  engine.on('problem', () => {});
  await engine.start({ timeoutMs: 3000 });
  const httpServer = http.createServer(createApp({ engine }));
  const realtime = attachRealtime({ httpServer, engine });
  httpServer.listen(0, '127.0.0.1');
  await once(httpServer, 'listening');
  const url = `http://127.0.0.1:${httpServer.address().port}`;
  const sockets = [];
  return {
    url,
    fake,
    async login(userId) {
      const socket = connect(url, { auth: { userId: String(userId) }, transports: ['websocket'], reconnection: false });
      sockets.push(socket);
      await waitEvent(socket, 'connect');
      return socket;
    },
    async cleanup() {
      for (const s of sockets) s.close();
      await realtime.close();
      httpServer.closeAllConnections();
      await engine.stop();
      await fake.close();
    },
  };
}

// INFY is symbol 2; buyer 11 and seller 22 by default
const tradeEvent = (o = {}) =>
  eventFrame({
    type: EventType.TRADE,
    symbol: 2,
    clientRef: 1n,
    trade: {
      tradeId: 99n,
      symbol: 2,
      price: 50005,
      qty: 17,
      buyOrderId: makeOrderId(2, 5),
      sellOrderId: makeOrderId(2, 6),
      buyerId: 11,
      sellerId: 22,
      seq: 1n,
      ...o.trade,
    },
  });

test('a connection without a login is refused', async () => {
  const t = await setup();
  try {
    const socket = connect(t.url, { auth: {}, transports: ['websocket'], reconnection: false });
    const err = await waitEvent(socket, 'connect_error');
    assert.equal(err.message, 'UNAUTHENTICATED');
    socket.close();
  } finally {
    await t.cleanup();
  }
});

test('subscribe checks the symbol list and normalises it', async () => {
  const t = await setup();
  try {
    const s = await t.login(7);
    const tooMany = ['INFY', 'TCS', 'SBIN', 'RELIANCE', 'HDFCBANK', 'ICICIBANK', 'INFY'];
    for (const bad of ['INFY', [], ['NOPE'], ['INFY', 'NOPE'], [3], null, undefined, tooMany]) {
      const r = await rpc(s, 'subscribe', bad);
      assert.equal(r.ok, false, JSON.stringify(bad));
      assert.equal(r.error, 'INVALID_SYMBOL');
    }
    const good = await rpc(s, 'subscribe', ['reliance', 'tcs', 'TCS']);
    assert.equal(good.ok, true);
    assert.deepEqual(good.symbols, ['RELIANCE', 'TCS']);
  } finally {
    await t.cleanup();
  }
});

test('a trade reaches subscribers of that symbol only, and carries no account ids', async () => {
  const t = await setup();
  try {
    const a = await t.login(1);
    const b = await t.login(2);
    await rpc(a, 'subscribe', ['INFY']);
    await rpc(b, 'subscribe', ['TCS']);
    const gotA = [];
    const gotB = [];
    a.on('trade', (x) => gotA.push(x));
    b.on('trade', (x) => gotB.push(x));

    t.fake.send(tradeEvent());
    await waitFor(() => gotA.length === 1);
    await sleep(100); // give a wrong delivery time to show up
    assert.equal(gotB.length, 0);

    const trade = gotA[0];
    assert.equal(trade.symbol, 'INFY');
    assert.equal(trade.price, '500.05');
    assert.equal(trade.quantity, 17);
    assert.equal(trade.tradeId, '99');
    assert.equal(typeof trade.time, 'number');
    assert.deepEqual(Object.keys(trade).sort(), ['price', 'quantity', 'symbol', 'time', 'tradeId']);
  } finally {
    await t.cleanup();
  }
});

test('fills go privately to the buyer and the seller only', async () => {
  const t = await setup();
  try {
    const buyer = await t.login(11);
    const seller = await t.login(22);
    const bystander = await t.login(33);
    await rpc(bystander, 'subscribe', ['INFY']); // watching the symbol does not show anyone's fills
    const fills = { 11: [], 22: [], 33: [] };
    buyer.on('fill', (f) => fills[11].push(f));
    seller.on('fill', (f) => fills[22].push(f));
    bystander.on('fill', (f) => fills[33].push(f));

    t.fake.send(tradeEvent());
    await waitFor(() => fills[11].length === 1 && fills[22].length === 1);
    await sleep(100);
    assert.equal(fills[33].length, 0);

    assert.deepEqual(Object.keys(fills[11][0]).sort(), ['orderId', 'price', 'quantity', 'side', 'symbol', 'time', 'tradeId']);
    assert.equal(fills[11][0].side, 'buy');
    assert.equal(fills[11][0].orderId, String(makeOrderId(2, 5)));
    assert.equal(fills[11][0].price, '500.05');
    assert.equal(fills[11][0].quantity, 17);
    assert.equal(fills[11][0].symbol, 'INFY');
    assert.equal(fills[22][0].side, 'sell');
    assert.equal(fills[22][0].orderId, String(makeOrderId(2, 6)));
  } finally {
    await t.cleanup();
  }
});

test('a self-trade gives the account a fill on each side', async () => {
  const t = await setup();
  try {
    const s = await t.login(11);
    const fills = [];
    s.on('fill', (f) => fills.push(f));
    t.fake.send(tradeEvent({ trade: { buyerId: 11, sellerId: 11 } }));
    await waitFor(() => fills.length === 2);
    assert.deepEqual(fills.map((f) => f.side).sort(), ['buy', 'sell']);
  } finally {
    await t.cleanup();
  }
});

test('order updates go to the owner only', async () => {
  const t = await setup();
  try {
    const owner = await t.login(11);
    const other = await t.login(22);
    const orders = [];
    const cancels = [];
    const otherSaw = [];
    owner.on('order', (o) => orders.push(o));
    owner.on('order:cancelled', (c) => cancels.push(c));
    other.onAny((name) => otherSaw.push(name));

    const id = makeOrderId(2, 5);
    t.fake.send(eventFrame({
      type: EventType.ORDER_ACCEPTED, status: OrderStatus.NEW, side: Side.BUY, symbol: 2, user: 11,
      orderId: id, price: 50005, qty: 10, remaining: 10, clientRef: 1n,
    }));
    t.fake.send(eventFrame({
      type: EventType.ORDER_CANCELLED, status: OrderStatus.CANCELLED, symbol: 2, user: 11,
      orderId: id, clientRef: 2n,
    }));
    await waitFor(() => orders.length === 1 && cancels.length === 1);
    await sleep(100);

    assert.deepEqual(orders[0], {
      orderId: String(id), symbol: 'INFY', side: 'buy', price: '500.05', quantity: 10, remaining: 10, status: 'NEW',
    });
    assert.deepEqual(cancels[0], { orderId: String(id), symbol: 'INFY' });
    assert.deepEqual(otherSaw, []);
  } finally {
    await t.cleanup();
  }
});

test('subscribing replays the last 100 trades, oldest first', async () => {
  const t = await setup();
  try {
    const probe = await t.login(1); // counts arrivals, so we know when the gateway has seen them all
    await rpc(probe, 'subscribe', ['INFY']);
    let seen = 0;
    probe.on('trade', () => seen++);

    for (let i = 1; i <= 105; i++) t.fake.send(tradeEvent({ trade: { tradeId: BigInt(i) } }));
    await waitFor(() => seen === 105);

    const late = await t.login(2);
    const reply = await rpc(late, 'subscribe', ['INFY', 'TCS']);
    assert.equal(reply.recent.INFY.length, 100);
    assert.equal(reply.recent.INFY[0].tradeId, '6'); // the first five fell off the end
    assert.equal(reply.recent.INFY[99].tradeId, '105');
    assert.deepEqual(reply.recent.TCS, []);
  } finally {
    await t.cleanup();
  }
});

test('unsubscribe stops the trades', async () => {
  const t = await setup();
  try {
    const s = await t.login(1);
    await rpc(s, 'subscribe', ['INFY']);
    const got = [];
    s.on('trade', (x) => got.push(x));
    t.fake.send(tradeEvent());
    await waitFor(() => got.length === 1);

    const reply = await rpc(s, 'unsubscribe', ['INFY']);
    assert.equal(reply.ok, true);
    t.fake.send(tradeEvent());
    await sleep(150);
    assert.equal(got.length, 1);
  } finally {
    await t.cleanup();
  }
});

test('an order placed over REST shows up on the owner\'s socket', async () => {
  const t = await setup((cmd) => [
    eventFrame({
      type: EventType.ORDER_ACCEPTED, status: OrderStatus.NEW, side: cmd.side, symbol: cmd.symbol,
      user: cmd.user, orderId: makeOrderId(cmd.symbol, 5), price: cmd.price, qty: cmd.qty,
      remaining: cmd.qty, clientRef: cmd.clientRef,
    }),
  ]);
  try {
    const s = await t.login(7);
    const got = [];
    s.on('order', (o) => got.push(o));
    const res = await fetch(`${t.url}/api/orders`, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-user-id': '7' },
      body: JSON.stringify({ symbol: 'INFY', side: 'buy', type: 'limit', price: '500.05', quantity: 10 }),
    });
    const json = await res.json();
    assert.equal(res.status, 201);
    await waitFor(() => got.length === 1);
    assert.equal(got[0].orderId, json.orderId);
    assert.equal(got[0].status, 'NEW');
  } finally {
    await t.cleanup();
  }
});