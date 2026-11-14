import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bookView } from '../src/lib/book.js';

const level = (price, quantity) => ({ price, quantity });

test('depth bars scale to the largest quantity on either side', () => {
  const v = bookView({ bids: [level('2499.95', 50)], asks: [level('2500.05', 100)] });
  assert.equal(v.asks[0].pct, 100);
  assert.equal(v.bids[0].pct, 50);
});

test('a tiny quantity still gets a visible bar', () => {
  const v = bookView({ bids: [level('1.00', 1)], asks: [level('2.00', 1000)] });
  assert.equal(v.bids[0].pct, 2);
});

test('asks are listed highest first so the best ask sits next to the spread', () => {
  const v = bookView({
    bids: [level('99.95', 1), level('99.90', 1)],
    asks: [level('100.05', 1), level('100.10', 1)],
  });
  assert.deepEqual(v.asks.map((l) => l.price), ['100.10', '100.05']);
  assert.deepEqual(v.bids.map((l) => l.price), ['99.95', '99.90']);
});

test('the spread is exact, and absent when a side is empty', () => {
  assert.equal(bookView({ bids: [level('2499.95', 1)], asks: [level('2500.05', 1)] }).spread, '0.10');
  assert.equal(bookView({ bids: [], asks: [level('2500.05', 1)] }).spread, null);
  assert.equal(bookView({ bids: [level('2499.95', 1)], asks: [] }).spread, null);
  assert.deepEqual(bookView({ bids: [], asks: [] }), { asks: [], bids: [], spread: null });
});