import { TABLES, configFromEnv, createPool } from '../src/db.js';

const config = configFromEnv();
const pool = createPool(config);
try {
  const [[info]] = await pool.query(
    'SELECT VERSION() AS version, DATABASE() AS db, @@character_set_database AS charset, @@sql_mode AS mode',
  );
  console.log(`connected to MySQL ${info.version} as ${config.user}@${config.host}:${config.port}`);
  console.log(`database "${info.db}", character set ${info.charset}`);
  console.log(`strict mode: ${/STRICT_TRANS_TABLES|STRICT_ALL_TABLES/.test(info.mode) ? 'on' : 'OFF (invalid values would be silently changed)'}`);
  const [rows] = await pool.query('SHOW TABLES');
  const present = rows.map((row) => Object.values(row)[0]);
  for (const table of TABLES) {
    if (!present.includes(table)) {
      console.log(`  missing  ${table}  (run: npm run db:migrate)`);
      continue;
    }
    const [[count]] = await pool.query(`SELECT COUNT(*) AS n FROM \`${table}\``);
    console.log(`  present  ${table}  (${count.n} rows)`);
  }
} catch (err) {
  console.error('database check failed:', err.message);
  process.exitCode = 1;
} finally {
  await pool.end();
}