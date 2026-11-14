import { test, before, after, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { TABLES, configFromEnv, createPool, migrate } from '../../src/db.js';
import { makeOrderId } from '../../src/protocol.js';

// These tests run against the separate exchange_test database and empty it before every test.
let pool;

before(async () => {
  pool = createPool({ ...configFromEnv(), database: 'exchange_test' });
  await migrate(pool);
});
after(async () => {
  await pool.end();
});
beforeEach(async () => {
  for (const table of ['trades', 'orders', 'engine_runs']) await pool.query(`TRUNCATE TABLE ${table}`);
});

const TRADE_COLUMNS =
  '(engine_id, trade_id, symbol, price_paise, quantity, buy_order_id, sell_order_id, buyer_id, seller_id, seq)';
const tradeRow = (engineId, tradeId) => [engineId, tradeId, 0, 250005, 10, '1', '2', 5, 6, '1'];
const insertTrade = (engineId, tradeId) =>
  pool.execute(`INSERT INTO trades ${TRADE_COLUMNS} VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, tradeRow(engineId, tradeId));

test('migrate creates every table, and running it again keeps the data', async () => {
  const [tables] = await pool.query('SHOW TABLES');
  const names = tables.map((row) => Object.values(row)[0]);
  for (const table of TABLES) assert.ok(names.includes(table), `missing table ${table}`);

  await pool.execute('INSERT INTO engine_runs (engine_id) VALUES (?)', ['42']);
  await migrate(pool);
  const [rows] = await pool.query('SELECT engine_id FROM engine_runs');
  assert.equal(rows.length, 1);
  assert.equal(rows[0].engine_id, '42');
});

test('ids beyond 2^53 come back exactly', async () => {
  const maxU64 = '18446744073709551615';
  const big = String(makeOrderId(40, 5)); // above 2^53: a JavaScript number would lose digits
  assert.ok(BigInt(big) > BigInt(Number.MAX_SAFE_INTEGER));
  await pool.execute('INSERT INTO engine_runs (engine_id) VALUES (?)', [maxU64]);
  await pool.execute(
    `INSERT INTO orders (engine_id, order_id, user_id, symbol, side, price_paise, quantity, status)
     VALUES (?, ?, 1, 40, 'buy', 250000, 10, 'NEW')`,
    [maxU64, big],
  );
  const [rows] = await pool.query('SELECT engine_id, order_id FROM orders');
  assert.equal(rows[0].engine_id, maxU64);
  assert.equal(rows[0].order_id, big);
});

test('the same trade id is refused twice in one engine run, but fine in another run', async () => {
  await pool.query('INSERT INTO engine_runs (engine_id) VALUES (1), (2)');
  await insertTrade('1', '7');
  await assert.rejects(insertTrade('1', '7'), { code: 'ER_DUP_ENTRY' });
  await insertTrade('2', '7'); // a fresh journal numbers its trades from 1 again: a different run
  const [[row]] = await pool.query('SELECT COUNT(*) AS n FROM trades');
  assert.equal(Number(row.n), 2);
});

test('prices and quantities are stored as exact integers', async () => {
  await pool.query('INSERT INTO engine_runs (engine_id) VALUES (1)');
  await insertTrade('1', '1');
  const [rows] = await pool.query('SELECT price_paise, quantity FROM trades');
  assert.equal(rows[0].price_paise, '250005'); // Rs 2,500.05 in paise, no decimals anywhere
  assert.equal(rows[0].quantity, '10');
});

test('a thousand rows go in with one statement', async () => {
  await pool.query('INSERT INTO engine_runs (engine_id) VALUES (1)');
  const rows = Array.from({ length: 1000 }, (_, i) => tradeRow('1', String(i + 1)));
  const [result] = await pool.query(`INSERT INTO trades ${TRADE_COLUMNS} VALUES ?`, [rows]);
  assert.equal(result.affectedRows, 1000);
  const [[row]] = await pool.query('SELECT COUNT(*) AS n FROM trades');
  assert.equal(Number(row.n), 1000);
});

test('values outside the allowed lists are refused, not silently changed', async () => {
  await pool.query('INSERT INTO engine_runs (engine_id) VALUES (1)');
  const insert = (side, status) =>
    pool.execute(
      `INSERT INTO orders (engine_id, order_id, user_id, symbol, side, price_paise, quantity, status)
       VALUES (1, 1, 1, 0, ?, 250000, 10, ?)`,
      [side, status],
    );
  await assert.rejects(insert('sideways', 'NEW'));
  await assert.rejects(insert('buy', 'BOGUS'));
  await insert('buy', 'NEW'); // and a valid row still goes in
});