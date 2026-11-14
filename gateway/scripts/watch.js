import { io } from 'socket.io-client';

const [userId = '0', ...names] = process.argv.slice(2);
const symbols = names.length > 0 ? names : ['RELIANCE'];
const label = `[account ${userId}]`;
const socket = io(process.env.GATEWAY ?? 'http://127.0.0.1:3000', { auth: { userId } });

socket.on('connect', () => {
  console.log(label, 'connected');
  socket.emit('subscribe', symbols, (reply) => console.log(label, 'subscribed:', JSON.stringify(reply)));
});
socket.on('connect_error', (err) => console.error(label, 'connect error:', err.message));
for (const name of ['trade', 'order', 'order:cancelled', 'fill']) {
  socket.on(name, (data) => console.log(label, name.padEnd(16), JSON.stringify(data)));
}