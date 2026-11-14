import net from 'node:net';
import { once } from 'node:events';
import { FrameReader, decodeCommandFrame } from '../src/protocol.js';

/**
 * A fake engine. onCommand(command) returns an array of event frames (Buffers) to send back.
 * Every decoded command is recorded in `commands`. send(frame) pushes an event to the
 * connected client at any time.
 */
export async function startFakeEngine(onCommand = () => []) {
  const sockets = new Set();
  const commands = [];
  const server = net.createServer((socket) => {
    sockets.add(socket);
    const reader = new FrameReader();
    socket.setNoDelay(true);
    socket.on('data', (chunk) => {
      for (const frame of reader.push(chunk)) {
        const command = decodeCommandFrame(frame);
        commands.push(command);
        for (const reply of onCommand(command)) socket.write(reply);
      }
    });
    socket.on('error', () => {});
    socket.on('close', () => sockets.delete(socket));
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return {
    port: server.address().port,
    commands,
    send(frame) {
      for (const s of sockets) s.write(frame);
    },
    async close() {
      for (const s of sockets) s.destroy();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}