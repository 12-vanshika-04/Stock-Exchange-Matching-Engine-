import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MAX_FILLS, initialOrders, ordersReducer, selectOrders } from '../src/lib/orders.js';

const accepted = (o) => ({
  type: 'accepted',
  order: { orderId: '1', symbol: 'INFY', side: 'buy', price: '1500.00', quantity: 10, remaining: 10, status: 'NEW', ...o },
});
const fill = (o) => ({
  type: 'fill',
  fill: { orderId: '1', symbol: 'INFY', side: 'buy', price: '1500.00', quantity: 4, tradeId: 't1', time: 1, ...o },
});
const run = (...actions) => actions.reduce(ordersReducer, initialOrders());
const first = (state) => selectOrders(state)[0];

test('orders are listed newest first, a new one is open with nothing filled', () => {
  const s = run(accepted({ orderId: '1' }), accepted({ orderId: '2' }));
  assert.deepEqual(selectOrders(s).map((o) => o.orderId), ['2', '1']);
  const o = first(s);
  assert.equal(o.status, 'NEW');
  assert.equal(o.filled, 0);
  assert.equal(o.remaining, 10);
  assert.equal(o.open, true);
});

test('the same order reported twice (REST reply and socket event) is listed once', () => {
  const s = run(accepted(), accepted());
  assert.equal(selectOrders(s).length, 1);
});

test('a resting order moves from NEW to PARTIAL to FILLED as others trade with it', () => {
  let s = run(accepted(), fill({ tradeId: 't1', quantity: 4 }));
  assert.equal(first(s).status, 'PARTIAL');
  assert.equal(first(s).filled, 4);
  assert.equal(first(s).remaining, 6);
  assert.equal(first(s).open, true);
  s = ordersReducer(s, fill({ tradeId: 't2', quantity: 6 }));
  assert.equal(first(s).status, 'FILLED');
  assert.equal(first(s).remaining, 0);
  assert.equal(first(s).open, false);
});

test('an order accepted as already filled is not counted twice when its fills follow', () => {
  const justAccepted = run(accepted({ status: 'FILLED', remaining: 0 }));
  assert.equal(first(justAccepted).status, 'FILLED'); // right away, before any fill arrives
  const s = ordersReducer(
    ordersReducer(justAccepted, fill({ tradeId: 't1', quantity: 6 })),
    fill({ tradeId: 't2', quantity: 4 }),
  );
  assert.equal(first(s).filled, 10); // not 20
  assert.equal(first(s).status, 'FILLED');
});

test('an order partly filled on arrival keeps counting correctly', () => {
  const s = run(
    accepted({ status: 'PARTIAL', remaining: 7 }), // 3 filled at once, the rest rests
    fill({ tradeId: 't1', quantity: 3 }),
    fill({ tradeId: 't2', quantity: 4 }), // later, someone else's order hits it
  );
  assert.equal(first(s).filled, 7);
  assert.equal(first(s).remaining, 3);
  assert.equal(first(s).status, 'PARTIAL');
});

test('the same fill delivered twice counts once', () => {
  const s = run(accepted(), fill({ tradeId: 't1', quantity: 4 }), fill({ tradeId: 't1', quantity: 4 }));
  assert.equal(first(s).filled, 4);
  assert.equal(s.fills.length, 1);
});

test('cancelling keeps what was already filled, and cancelling again changes nothing', () => {
  const s = run(accepted(), fill({ quantity: 4 }), { type: 'cancelled', orderId: '1' });
  assert.equal(first(s).status, 'CANCELLED');
  assert.equal(first(s).filled, 4);
  assert.equal(first(s).open, false);
  assert.equal(ordersReducer(s, { type: 'cancelled', orderId: '1' }), s);
});

test('a market order whose unfilled part was cancelled shows what did fill', () => {
  const s = run(accepted({ status: 'CANCELLED', price: '0.00', quantity: 10, remaining: 6 }), fill({ tradeId: 't1', quantity: 4 }));
  assert.equal(first(s).status, 'CANCELLED');
  assert.equal(first(s).filled, 4);
  assert.equal(first(s).open, false);
});

test('a fill for an order this page never saw is kept as a fill but creates no order', () => {
  const s = run(fill({ orderId: '99' }));
  assert.equal(selectOrders(s).length, 0);
  assert.equal(s.fills.length, 1);
});

test('only the newest fills are kept', () => {
  const actions = Array.from({ length: MAX_FILLS + 20 }, (_, i) => fill({ orderId: '99', tradeId: `t${i}` }));
  const s = run(...actions);
  assert.equal(s.fills.length, MAX_FILLS);
  assert.equal(s.fills[0].tradeId, `t${MAX_FILLS + 19}`);
});

test('clear forgets everything', () => {
  const s = ordersReducer(run(accepted(), fill({})), { type: 'clear' });
  assert.deepEqual(s, initialOrders());
});