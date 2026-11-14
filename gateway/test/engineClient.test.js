import { test } from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import { once } from 'node:events';
import { EngineClient } from '../src/engineClient.js';
import { EventType, OrderStatus, OrderType, ProtocolError, Side, encodeCommand } from '../src/protocol.js';
import { eventFrame } from './helpers.js';

const order = {
  kind: 'new',
  side: Side.BUY,
  orderType: OrderType.LIMIT,
  symbol: 0,
  user: 0,
  price: 50000,
  qty: 1,
  clientRef: 1n,
};
const { clientRef: _unused, ...orderNoRef } = order;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(condition, ms = 3000) {
  const end = Date.now() + ms;
  while (!condition()) {
    if (Date.now() > end) throw new Error('timed out waiting for a condition');
    await sleep(5);
  }
}

// A fake engine: a TCP server that records the bytes the client sends.
async function startFake() {
  const sockets = new Set();
  const chunks = [];
  const server = net.createServer((socket) => {
    sockets.add(socket);
    socket.setNoDelay(true);
    socket.on('data', (c) => chunks.push(c));
    socket.on('error', () => {});
    socket.on('close', () => sockets.delete(socket));
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return {
    port: server.address().port,
    sockets,
    received: () => Buffer.concat(chunks),
    async close() {
      for (const s of sockets) s.destroy();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}

async function setup() {
  const fake = await startFake();
  const client = new EngineClient({ port: fake.port, minDelayMs: 10, maxDelayMs: 50 });
  client.on('problem', () => {}); // some tests provoke problems on purpose
  await client.start({ timeoutMs: 3000 });
  await waitFor(() => fake.sockets.size === 1);
  return {
    fake,
    client,
    async cleanup() {
      await client.stop();
      await fake.close();
    },
  };
}

test('send() writes the framed command bytes', async () => {
  const t = await setup();
  try {
    t.client.send(order);
    await waitFor(() => t.fake.received().length >= 37);
    assert.deepEqual(t.fake.received(), encodeCommand(order));
  } finally {
    await t.cleanup();
  }
});

test('events are decoded even when they arrive in pieces', async () => {
  const t = await setup();
  try {
    const events = [];
    t.client.on('event', (e) => events.push(e));
    const frames = Buffer.concat([
      eventFrame({ type: EventType.ORDER_ACCEPTED, clientRef: 7n, orderId: 100n, qty: 10, remaining: 10, price: 50000 }),
      eventFrame({ type: EventType.TRADE, clientRef: 7n }),
    ]);
    const [sock] = t.fake.sockets;
    sock.write(frames.subarray(0, 50)); // ends in the middle of the first event
    await sleep(30);
    sock.write(frames.subarray(50));
    await waitFor(() => events.length === 2);
    assert.equal(events[0].type, EventType.ORDER_ACCEPTED);
    assert.equal(events[0].orderId, 100n);
    assert.equal(events[0].remaining, 10);
    assert.equal(events[1].type, EventType.TRADE);
  } finally {
    await t.cleanup();
  }
});

test('request() resolves with its own acknowledgement and ignores trades and other refs', async () => {
  const t = await setup();
  try {
    const reply = t.client.request(orderNoRef, { timeoutMs: 3000 });
    await waitFor(() => t.fake.received().length >= 37);
    const ref = t.fake.received().readBigUInt64LE(29); // the clientRef the client assigned
    const [sock] = t.fake.sockets;
    sock.write(
      Buffer.concat([
        eventFrame({ type: EventType.ORDER_ACCEPTED, clientRef: ref + 1n }), // someone else's reply
        eventFrame({ type: EventType.TRADE, clientRef: ref }), // a trade is not the acknowledgement
        eventFrame({ type: EventType.ORDER_ACCEPTED, clientRef: ref, status: OrderStatus.PARTIAL, remaining: 7 }),
      ]),
    );
    const ack = await reply;
    assert.equal(ack.clientRef, ref);
    assert.equal(ack.type, EventType.ORDER_ACCEPTED);
    assert.equal(ack.status, OrderStatus.PARTIAL);
    assert.equal(ack.remaining, 7);
  } finally {
    await t.cleanup();
  }
});

test('request() rejects when no reply arrives in time', async () => {
  const t = await setup();
  try {
    await assert.rejects(t.client.request(orderNoRef, { timeoutMs: 50 }), { code: 'ETIMEDOUT' });
  } finally {
    await t.cleanup();
  }
});

test('the client reconnects after the engine drops the connection, and works again', async () => {
  const t = await setup();
  try {
    let connected = 0;
    t.client.on('connected', () => connected++);
    const [first] = t.fake.sockets;
    first.destroy();
    await waitFor(() => connected === 1 && t.fake.sockets.size === 1 && !t.fake.sockets.has(first));
    t.client.send(order);
    await waitFor(() => t.fake.received().length >= 37);
    assert.deepEqual(t.fake.received(), encodeCommand(order));
  } finally {
    await t.cleanup();
  }
});

test('a protocol violation from the engine drops the connection and the client reconnects', async () => {
  const t = await setup();
  try {
    const problems = [];
    let connected = 0;
    t.client.on('problem', (e) => problems.push(e));
    t.client.on('connected', () => connected++);
    const [first] = t.fake.sockets;
    first.write(Buffer.from([0, 0, 0, 0])); // a frame length of zero
    await waitFor(() => connected === 1 && problems.length >= 1);
    assert.ok(problems.some((e) => e instanceof ProtocolError));
  } finally {
    await t.cleanup();
  }
});

test('a request in flight is rejected at once when the connection drops', async () => {
  const t = await setup();
  try {
    const reply = t.client.request(orderNoRef, { timeoutMs: 3000 });
    await waitFor(() => t.fake.received().length >= 37);
    [...t.fake.sockets][0].destroy();
    await assert.rejects(reply, { code: 'ECONNLOST' });
  } finally {
    await t.cleanup();
  }
});

test('send() and request() fail clearly when there is no connection', async () => {
  const client = new EngineClient({ port: 1 }); // never started
  assert.throws(() => client.send(order), { code: 'ENGINE_UNAVAILABLE' });
  await assert.rejects(client.request(orderNoRef), { code: 'ENGINE_UNAVAILABLE' });
});