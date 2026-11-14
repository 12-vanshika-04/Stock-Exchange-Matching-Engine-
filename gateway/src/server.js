import http from 'node:http';
import { EngineClient } from './engineClient.js';
import { createApp } from './app.js';
import { attachRealtime } from './realtime.js';

const httpPort = Number(process.env.PORT ?? 3000);
const engine = new EngineClient({
  host: process.env.ENGINE_HOST ?? '127.0.0.1',
  port: Number(process.env.ENGINE_PORT ?? 9000),
});
engine.on('connected', () => console.log('engine connected'));
engine.on('disconnected', () => console.log('engine disconnected, retrying'));
engine.on('problem', (err) => console.error('engine problem:', err.message));
engine.start().catch(() => {}); // the server starts even if the engine is not up yet

const httpServer = http.createServer(createApp({ engine }));
const realtime = attachRealtime({ httpServer, engine });

// Loopback only, because the development login trusts a header.
httpServer.listen(httpPort, '127.0.0.1', () => {
  console.log(`gateway listening on http://127.0.0.1:${httpPort} (development login: x-user-id / auth.userId)`);
});

let closing = false;
async function shutdown() {
  if (closing) return;
  closing = true;
  console.log('shutting down');
  await realtime.close();
  httpServer.closeAllConnections();
  await engine.stop();
  process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);