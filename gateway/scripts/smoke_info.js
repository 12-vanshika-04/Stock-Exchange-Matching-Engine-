import { EngineClient } from '../src/engineClient.js';

const port = Number(process.argv[2] ?? 9000);
const client = new EngineClient({ port });
client.on('problem', (err) => console.error('problem:', err.message));

let failures = 0;
function check(name, ok, detail = '') {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!ok) failures++;
}

try {
  await client.start({ timeoutMs: 5000 });
  const first = await client.engineId();
  const second = await client.engineId();
  console.log(`engine id ${first.engineId}, ${first.symbols} symbols\n`);
  check('the engine has an identity', first.engineId !== '0');
  check('the id is a whole number below 2^52', /^\d+$/.test(first.engineId) && BigInt(first.engineId) < 2n ** 52n);
  check('it is the same on every question', first.engineId === second.engineId);
  check('the engine serves six symbols', first.symbols === 6);
} catch (err) {
  console.error('\nsmoke test aborted:', err.message);
  failures++;
} finally {
  await client.stop();
}

console.log(failures === 0 ? '\nAll checks passed.' : `\n${failures} check(s) FAILED`);
process.exitCode = failures === 0 ? 0 : 1;
