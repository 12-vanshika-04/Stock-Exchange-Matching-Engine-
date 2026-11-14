import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateOrder } from '../src/lib/orderForm.js';

test('a valid limit order passes and keeps the price text exactly', () => {
  assert.deepEqual(validateOrder({ side: 'buy', type: 'limit', price: '2500.05', quantity: '10' }), {
    ok: true,
    order: { side: 'buy', type: 'limit', price: '2500.05', quantity: 10 },
  });
});

test('a market order ignores the price field', () => {
  assert.deepEqual(validateOrder({ side: 'sell', type: 'market', price: 'garbage', quantity: ' 3 ' }), {
    ok: true,
    order: { side: 'sell', type: 'market', quantity: 3 },
  });
});

test('bad quantities are refused', () => {
  for (const quantity of ['', '0', '01', '1.5', 'abc', '-3', '1e3', '100000001']) {
    const r = validateOrder({ side: 'buy', type: 'market', price: '', quantity });
    assert.equal(r.ok, false, `"${quantity}"`);
    assert.ok(r.errors.quantity, `"${quantity}"`);
  }
  assert.equal(validateOrder({ side: 'buy', type: 'market', price: '', quantity: '100000000' }).ok, true);
});

test('bad limit prices are refused', () => {
  for (const price of ['', '0', '0.00', '500.123', 'abc', '-5', '1e3', '.5', '5.']) {
    const r = validateOrder({ side: 'buy', type: 'limit', price, quantity: '1' });
    assert.equal(r.ok, false, `"${price}"`);
    assert.ok(r.errors.price, `"${price}"`);
  }
});

test('both problems are reported together', () => {
  const r = validateOrder({ side: 'buy', type: 'limit', price: '', quantity: '0' });
  assert.equal(r.ok, false);
  assert.ok(r.errors.price);
  assert.ok(r.errors.quantity);
});