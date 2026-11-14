import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MAX_TRADES, emptyTrades, paiseOf, priceMove, tradesReducer } from '../src/lib/trades.js';

const SYMBOLS = ['AAA', 'BBB'];
const trade = (symbol, id, price = '100.00') => ({ symbol, tradeId: String(id), price, quantity: 1, time: id });

test('a snapshot is shown newest first', () => {
  const state = tradesReducer(emptyTrades(SYMBOLS), {
    type: 'snapshot',
    recent: { AAA: [trade('AAA', 1), trade('AAA', 2), trade('AAA', 3)], BBB: [] },
  });
  assert.deepEqual(state.AAA.map((t) => t.tradeId), ['3', '2', '1']);
  assert.deepEqual(state.BBB, []);
});

test('a snapshot keeps only the newest 100 trades', () => {
  const recent = Array.from({ length: 150 }, (_, i) => trade('AAA', i + 1));
  const state = tradesReducer(emptyTrades(SYMBOLS), { type: 'snapshot', recent: { AAA: recent } });
  assert.equal(state.AAA.length, MAX_TRADES);
  assert.equal(state.AAA[0].tradeId, '150');
  assert.equal(state.AAA[99].tradeId, '51');
});

test('a live trade goes to the top of its own symbol only', () => {
  let state = emptyTrades(SYMBOLS);
  state = tradesReducer(state, { type: 'trade', trade: trade('AAA', 1) });
  state = tradesReducer(state, { type: 'trade', trade: trade('AAA', 2) });
  state = tradesReducer(state, { type: 'trade', trade: trade('BBB', 3) });
  assert.deepEqual(state.AAA.map((t) => t.tradeId), ['2', '1']);
  assert.deepEqual(state.BBB.map((t) => t.tradeId), ['3']);
});

test('the list never grows past the cap', () => {
  let state = emptyTrades(SYMBOLS);
  for (let i = 1; i <= MAX_TRADES + 25; i++) state = tradesReducer(state, { type: 'trade', trade: trade('AAA', i) });
  assert.equal(state.AAA.length, MAX_TRADES);
  assert.equal(state.AAA[0].tradeId, String(MAX_TRADES + 25));
});

test('a trade for an unknown symbol is ignored, and so is a repeat of the latest trade', () => {
  let state = emptyTrades(SYMBOLS);
  assert.equal(tradesReducer(state, { type: 'trade', trade: trade('ZZZ', 1) }), state);
  state = tradesReducer(state, { type: 'trade', trade: trade('AAA', 1) });
  assert.equal(tradesReducer(state, { type: 'trade', trade: trade('AAA', 1) }), state);
});

test('clear empties every symbol and keeps the symbols', () => {
  let state = tradesReducer(emptyTrades(SYMBOLS), { type: 'trade', trade: trade('AAA', 1) });
  state = tradesReducer(state, { type: 'clear' });
  assert.deepEqual(state, { AAA: [], BBB: [] });
});

test('paiseOf reads decimal strings exactly', () => {
  assert.equal(paiseOf('2500.05'), 250005);
  assert.equal(paiseOf('500'), 50000);
  assert.equal(paiseOf('500.5'), 50050);
  assert.equal(paiseOf('0.05'), 5);
});

test('priceMove compares a trade with the one before it in time', () => {
  const list = [trade('AAA', 3, '101.00'), trade('AAA', 2, '100.00'), trade('AAA', 1, '100.00')]; // newest first
  assert.equal(priceMove(list, 0), 1); // up
  assert.equal(priceMove(list, 1), 0); // unchanged
  assert.equal(priceMove(list, 2), 0); // the oldest has nothing before it
  assert.equal(priceMove([trade('AAA', 2, '99.95'), trade('AAA', 1, '100.00')], 0), -1); // down
  assert.equal(priceMove([], 0), 0);
});