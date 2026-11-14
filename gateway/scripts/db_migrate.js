import { TABLES, configFromEnv, createPool, migrate } from '../src/db.js';

const config = configFromEnv();
const pool = createPool(config);
try {
  await migrate(pool);
  const [rows] = await pool.query('SHOW TABLES');
  const present = rows.map((row) => Object.values(row)[0]);
  console.log(`database "${config.database}":`);
  for (const table of TABLES) console.log(`  ${present.includes(table) ? 'ready  ' : 'MISSING'} ${table}`);
  if (!TABLES.every((table) => present.includes(table))) process.exitCode = 1;
} catch (err) {
  console.error('migration failed:', err.message);
  process.exitCode = 1;
} finally {
  await pool.end();
}