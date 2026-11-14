import { test } from 'node:test';
import assert from 'node:assert/strict';
import { formatPaise, formatRupees } from '../src/lib/format.js';

test('formatRupees groups digits the Indian way', () => {
  assert.equal(formatRupees('0.00'), '0.00');
  assert.equal(formatRupees('500.00'), '500.00');
  assert.equal(formatRupees('2500.00'), '2,500.00');
  assert.equal(formatRupees('123456.50'), '1,23,456.50');
  assert.equal(formatRupees('10000000.00'), '1,00,00,000.00');
  assert.equal(formatRupees('-1500.50'), '-1,500.50');
  assert.equal(formatRupees('1000'), '1,000');
});

test('formatPaise writes exact rupees', () => {
  assert.equal(formatPaise(10), '0.10');
  assert.equal(formatPaise(5), '0.05');
  assert.equal(formatPaise(250005), '2500.05');
  assert.equal(formatPaise(0), '0.00');
  assert.equal(formatPaise(-150), '-1.50');
});