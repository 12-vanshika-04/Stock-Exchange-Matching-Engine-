import { Server } from 'socket.io';
import { EventType } from './protocol.js';
import { paiseToRupees } from './money.js';
import { orderJson } from './format.js';
import { SYMBOLS, symbolId, symbolName } from './symbols.js';

const RECENT_TRADES = 100;
const userRoom = (id) => `user:${id}`;
const symbolRoom = (name) => `symbol:${name}`;

/**
 * DEVELOPMENT ONLY. Trusts auth.userId from the handshake, so anyone can listen as any
 * account. Replaced by real login (JWT) in a later step.
 */
export function devAuthenticateSocket(socket) {
  const raw = socket.handshake.auth?.userId;
  const s = typeof raw === 'number' ? String(raw) : raw;
  if (typeof s !== 'string' || !/^\d{1,9}$/.test(s)) return null;
  return { engineId: Number(s) };
}

// ['reliance', 'TCS', 'tcs'] -> ['RELIANCE', 'TCS']; null if anything is not a known symbol.
function canonicalSymbols(names) {
  if (!Array.isArray(names) || names.length < 1 || names.length > SYMBOLS.length) return null;
  const out = [];
  for (const n of names) {
    const id = typeof n === 'string' ? symbolId(n) : -1;
    if (id < 0) return null;
    if (!out.includes(SYMBOLS[id])) out.push(SYMBOLS[id]);
  }
  return out;
}

/**
 * Pushes engine events to browsers over Socket.io.
 *
 *   server -> client:  'trade' (public, per symbol)       { symbol, price, quantity, tradeId, time }
 *                      'fill' (private)                    { orderId, symbol, side, price, quantity, tradeId, time }
 *                      'order' (private, owner only)       { orderId, symbol, side, price, quantity, remaining, status }
 *                      'order:cancelled' (private)         { orderId, symbol }
 *   client -> server:  'subscribe'   (symbols[], ack)  ack({ ok, symbols, recent: { SYMBOL: [trade, ...] } })
 *                      'unsubscribe' (symbols[], ack)
 *
 * `time` is when the gateway received the event (the engine has no clock).
 */
export function attachRealtime({ httpServer, engine, authenticate = devAuthenticateSocket, now = Date.now, cors }) {
  const io = new Server(httpServer, { cors });
  const recent = new Map(SYMBOLS.map((name) => [name, []]));

  io.use((socket, next) => {
    const user = authenticate(socket);
    if (!user) {
      next(new Error('UNAUTHENTICATED'));
      return;
    }
    socket.data.user = user;
    next();
  });

  io.on('connection', (socket) => {
    socket.join(userRoom(socket.data.user.engineId));

    socket.on('subscribe', (names, ack) => {
      const reply = typeof ack === 'function' ? ack : () => {};
      const symbols = canonicalSymbols(names);
      if (!symbols) {
        reply({ ok: false, error: 'INVALID_SYMBOL', message: `send a list of: ${SYMBOLS.join(', ')}` });
        return;
      }
      const snapshot = {};
      for (const name of symbols) {
        socket.join(symbolRoom(name));
        snapshot[name] = recent.get(name).slice();
      }
      reply({ ok: true, symbols, recent: snapshot });
    });

    socket.on('unsubscribe', (names, ack) => {
      const reply = typeof ack === 'function' ? ack : () => {};
      const symbols = canonicalSymbols(names);
      if (!symbols) {
        reply({ ok: false, error: 'INVALID_SYMBOL' });
        return;
      }
      for (const name of symbols) socket.leave(symbolRoom(name));
      reply({ ok: true, symbols });
    });
  });

  function onTrade(t) {
    const symbol = symbolName(t.symbol);
    if (symbol === undefined) return; // an id this gateway does not know
    const time = now();
    const price = paiseToRupees(t.price);
    const tradeId = String(t.tradeId);

    const pub = { symbol, price, quantity: t.qty, tradeId, time };
    const list = recent.get(symbol);
    list.push(pub);
    if (list.length > RECENT_TRADES) list.shift();
    io.to(symbolRoom(symbol)).emit('trade', pub);

    const fill = { symbol, price, quantity: t.qty, tradeId, time };
    io.to(userRoom(t.buyerId)).emit('fill', { ...fill, side: 'buy', orderId: String(t.buyOrderId) });
    io.to(userRoom(t.sellerId)).emit('fill', { ...fill, side: 'sell', orderId: String(t.sellOrderId) });
  }

  function onEvent(e) {
    switch (e.type) {
      case EventType.TRADE:
        onTrade(e.trade);
        break;
      case EventType.ORDER_ACCEPTED:
        io.to(userRoom(e.user)).emit('order', orderJson(e));
        break;
      case EventType.ORDER_CANCELLED:
        io.to(userRoom(e.user)).emit('order:cancelled', {
          orderId: String(e.orderId),
          symbol: symbolName(e.symbol) ?? String(e.symbol),
        });
        break;
      default:
        break; // rejections are answered over HTTP; nothing to push
    }
  }
  engine.on('event', onEvent);

  return {
    io,
    /** Disconnects every client and closes the HTTP server. */
    close() {
      engine.off('event', onEvent);
      return new Promise((resolve) => io.close(() => resolve()));
    },
  };
}