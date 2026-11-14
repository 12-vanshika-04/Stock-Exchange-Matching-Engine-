import { test } from 'node:test';
import assert from 'node:assert/strict';
import { paiseToRupees, rupeesToPaise } from '../src/money.js';

test('rupeesToPaise accepts amounts with at most two decimals', () => {
  assert.equal(rupeesToPaise('500'), 50000);
  assert.equal(rupeesToPaise('500.05'), 50005);
  assert.equal(rupeesToPaise('500.5'), 50050);
  assert.equal(rupeesToPaise(500.05), 50005);
  assert.equal(rupeesToPaise('0.05'), 5);
  assert.equal(rupeesToPaise(0), 0);
});

test('rupeesToPaise refuses everything else', () => {
  const bad = ['', 'abc', '-1', '1.234', '1e3', ' 5', '5 ', '.5', '5.', '1,000', '9999999999',
    null, undefined, {}, [], true, 0.1 + 0.2, NaN, Infinity];
  for (const input of bad) assert.equal(rupeesToPaise(input), null, String(input));
});

test('paiseToRupees formats exactly', () => {
  assert.equal(paiseToRupees(50005), '500.05');
  assert.equal(paiseToRupees(5), '0.05');
  assert.equal(paiseToRupees(50000), '500.00');
  assert.equal(paiseToRupees(0), '0.00');
  assert.equal(paiseToRupees(-150), '-1.50');
});