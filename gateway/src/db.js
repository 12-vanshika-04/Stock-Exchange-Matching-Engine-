import mysql from 'mysql2/promise';

/** Reads the connection settings from the environment (scripts start with --env-file=.env). */
export function configFromEnv(env = process.env) {
  const missing = ['DB_USER', 'DB_PASSWORD', 'DB_NAME'].filter((key) => !env[key]);
  if (missing.length > 0) {
    throw new Error(`missing database settings: ${missing.join(', ')} (see .env.example)`);
  }
  return {
    host: env.DB_HOST ?? '127.0.0.1',
    port: Number(env.DB_PORT ?? 3306),
    user: env.DB_USER,
    password: env.DB_PASSWORD,
    database: env.DB_NAME,
  };
}

export function createPool(config, { connectionLimit = 10 } = {}) {
  return mysql.createPool({
    ...config,
    waitForConnections: true,
    connectionLimit,
    // Engine ids and order ids can exceed 2^53, where JavaScript numbers lose digits:
    // every BIGINT comes back as an exact string, and ids are passed in as strings too.
    supportBigNumbers: true,
    bigNumberStrings: true,
    timezone: 'Z', // store and read times as UTC
    charset: 'utf8mb4',
  });
}

export const TABLES = ['engine_runs', 'orders', 'trades'];

// engine_id identifies one lifetime of the engine's journal. Numbering (orders, trades)
// starts again at 1 with every fresh journal, so every row is keyed by (engine_id, id).
// There are no foreign keys on purpose: rows are written in batches and the engine, not
// the database, is the source of truth.
const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS engine_runs (
     engine_id  BIGINT UNSIGNED NOT NULL,
     first_seen DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
     PRIMARY KEY (engine_id)
   ) ENGINE=InnoDB`,

  `CREATE TABLE IF NOT EXISTS orders (
     engine_id       BIGINT UNSIGNED NOT NULL,
     order_id        BIGINT UNSIGNED NOT NULL,
     user_id         INT UNSIGNED NOT NULL,
     symbol          SMALLINT UNSIGNED NOT NULL,
     side            ENUM('buy', 'sell') NOT NULL,
     price_paise     BIGINT NOT NULL,
     quantity        BIGINT NOT NULL,
     filled_quantity BIGINT NOT NULL DEFAULT 0,
     status          ENUM('NEW', 'PARTIAL', 'FILLED', 'CANCELLED', 'REJECTED') NOT NULL,
     reject_reason   VARCHAR(32) NULL,
     created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
     updated_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
     PRIMARY KEY (engine_id, order_id),
     KEY orders_by_user (engine_id, user_id, created_at)
   ) ENGINE=InnoDB`,

  `CREATE TABLE IF NOT EXISTS trades (
     engine_id     BIGINT UNSIGNED NOT NULL,
     trade_id      BIGINT UNSIGNED NOT NULL,
     symbol        SMALLINT UNSIGNED NOT NULL,
     price_paise   BIGINT NOT NULL,
     quantity      BIGINT NOT NULL,
     buy_order_id  BIGINT UNSIGNED NOT NULL,
     sell_order_id BIGINT UNSIGNED NOT NULL,
     buyer_id      INT UNSIGNED NOT NULL,
     seller_id     INT UNSIGNED NOT NULL,
     seq           BIGINT UNSIGNED NOT NULL,
     executed_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
     PRIMARY KEY (engine_id, trade_id),
     KEY trades_by_symbol (engine_id, symbol, trade_id),
     KEY trades_by_buyer (engine_id, buyer_id, trade_id),
     KEY trades_by_seller (engine_id, seller_id, trade_id)
   ) ENGINE=InnoDB`,
];

/** Creates any missing tables. Safe to run as often as you like; it never drops or alters anything. */
export async function migrate(pool) {
  for (const statement of SCHEMA) await pool.query(statement);
}