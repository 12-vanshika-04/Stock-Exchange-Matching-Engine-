import express from 'express';
import { EventType, OrderType, RejectReason, Side, symbolOfOrderId } from './protocol.js';
import { paiseToRupees, rupeesToPaise } from './money.js';
import { nameOf, orderJson } from './format.js';
import { SYMBOLS, symbolId, symbolName } from './symbols.js';

const MAX_BOOK_DEPTH = 50; // the engine allows 200; the page never needs more
const MAX_QTY = 100_000_000; // mirrors the engine's limit; the engine stays the authority


const REJECT_TEXT = {
  UNKNOWN_USER: 'unknown account',
  UNKNOWN_SYMBOL: 'unknown symbol',
  BAD_QUANTITY: 'invalid quantity',
  BAD_PRICE: 'invalid price',
  INSUFFICIENT_FUNDS: 'not enough cash for this order',
  INSUFFICIENT_SHARES: 'not enough shares to sell',
  PRICE_OFF_TICK: 'price must be a multiple of Rs 0.05',
  PRICE_OUT_OF_BAND: 'price is outside the allowed band',
};


/**
 * DEVELOPMENT ONLY. Trusts an x-user-id header, so anyone can act as any account.
 * Replaced by real login (JWT) in a later step; createApp takes any authenticate function.
 */
export function devAuthenticate(req, res, next) {
  const raw = req.get('x-user-id');
  if (raw === undefined || !/^\d{1,9}$/.test(raw)) {
    res.status(401).json({ error: 'UNAUTHENTICATED', message: 'send an x-user-id header (development login)' });
    return;
  }
  req.user = { engineId: Number(raw) };
  next();
}

const wrap = (fn) => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

function badRequest(res, error, message) {
  res.status(400).json({ error, message });
}

/**
 * @param {{engine: import('./engineClient.js').EngineClient,
 *          authenticate?: import('express').RequestHandler,
 *          requestTimeoutMs?: number}} options
 */
export function createApp({ engine, authenticate = devAuthenticate, requestTimeoutMs = 5000 }) {
  const app = express();
  app.disable('x-powered-by');
  app.use(express.json({ limit: '10kb' }));

  app.get('/api/health', (req, res) => {
    res.json({ ok: true, engineConnected: engine.connected });
  });

  app.post('/api/orders', authenticate, wrap(async (req, res) => {
    const body = req.body;
    if (body === null || typeof body !== 'object' || Array.isArray(body)) {
      badRequest(res, 'INVALID_BODY', 'send a JSON object');
      return;
    }
    const symbol = typeof body.symbol === 'string' ? symbolId(body.symbol) : -1;
    if (symbol < 0) {
      badRequest(res, 'INVALID_SYMBOL', `symbol must be one of ${SYMBOLS.join(', ')}`);
      return;
    }
    let side;
    if (body.side === 'buy') side = Side.BUY;
    else if (body.side === 'sell') side = Side.SELL;
    else {
      badRequest(res, 'INVALID_SIDE', 'side must be "buy" or "sell"');
      return;
    }
    let orderType;
    if (body.type === 'limit') orderType = OrderType.LIMIT;
    else if (body.type === 'market') orderType = OrderType.MARKET;
    else {
      badRequest(res, 'INVALID_TYPE', 'type must be "limit" or "market"');
      return;
    }
    if (!Number.isInteger(body.quantity) || body.quantity < 1 || body.quantity > MAX_QTY) {
      badRequest(res, 'INVALID_QUANTITY', `quantity must be a whole number from 1 to ${MAX_QTY}`);
      return;
    }
    let price = 0; // market orders carry no price
    if (orderType === OrderType.LIMIT) {
      price = rupeesToPaise(body.price);
      if (price === null || price <= 0) {
        badRequest(res, 'INVALID_PRICE', 'price must be a positive amount with at most two decimals, e.g. "500.05"');
        return;
      }
    }

    const ack = await engine.request(
      { kind: 'new', side, orderType, symbol, user: req.user.engineId, price, qty: body.quantity },
      { timeoutMs: requestTimeoutMs },
    );
    if (ack.type === EventType.ORDER_REJECTED) {
      const reason = nameOf(RejectReason, ack.reason);
      res.status(reason === 'UNKNOWN_USER' ? 403 : 422).json({ error: reason, message: REJECT_TEXT[reason] ?? 'order rejected' });
      return;
    }
    res.status(201).json(orderJson(ack));
  }));

  app.delete('/api/orders/:orderId', authenticate, wrap(async (req, res) => {
    const raw = req.params.orderId;
    if (!/^\d{1,20}$/.test(raw) || BigInt(raw) >= 2n ** 64n) {
      badRequest(res, 'INVALID_ORDER_ID', 'the order id must be a number');
      return;
    }
    const orderId = BigInt(raw);
    const ack = await engine.request(
      { kind: 'cancel', symbol: symbolOfOrderId(orderId), user: req.user.engineId, orderId },
      { timeoutMs: requestTimeoutMs },
    );
    if (ack.type === EventType.ORDER_CANCELLED) {
      res.json({ orderId: String(orderId), status: 'CANCELLED' });
      return;
    }
    // Unknown order, or not yours: the same answer, so nothing leaks about other accounts.
    res.status(404).json({ error: 'ORDER_NOT_FOUND', message: 'no such open order for this account' });
  }));

  // The account's cash and holdings, straight from the engine (the source of truth).
  app.get('/api/account', authenticate, wrap(async (req, res) => {
    const events = await engine.query(
      { kind: 'queryAccount', user: req.user.engineId },
      { timeoutMs: requestTimeoutMs },
    );
    const balance = events.find((e) => e.type === EventType.ACCOUNT_BALANCE);
    if (!balance) throw new Error('the engine sent no balance');
    const positions = events
      .filter((e) => e.type === EventType.ACCOUNT_POSITION)
      .map((p) => ({
        symbol: symbolName(p.symbol) ?? String(p.symbol),
        quantity: p.qty,
        reserved: p.remaining,
        available: p.qty - p.remaining,
        averageCost: paiseToRupees(p.qty > 0 ? Math.round(p.price / p.qty) : 0),
        costBasis: paiseToRupees(p.price),
      }));
    res.json({
      cash: paiseToRupees(balance.price),
      reservedCash: paiseToRupees(balance.qty),
      availableCash: paiseToRupees(balance.price - balance.qty),
      positions,
    });
  }));

  // The order book, best prices first. Public market data, so no login is needed.
  app.get('/api/book/:symbol', wrap(async (req, res) => {
    const symbol = symbolId(req.params.symbol);
    if (symbol < 0) {
      res.status(404).json({ error: 'UNKNOWN_SYMBOL', message: `symbol must be one of ${SYMBOLS.join(', ')}` });
      return;
    }
    let depth = 10;
    if (req.query.depth !== undefined) {
      const raw = req.query.depth;
      if (typeof raw !== 'string' || !/^\d{1,3}$/.test(raw) || Number(raw) < 1 || Number(raw) > MAX_BOOK_DEPTH) {
        badRequest(res, 'INVALID_DEPTH', `depth must be a whole number from 1 to ${MAX_BOOK_DEPTH}`);
        return;
      }
      depth = Number(raw);
    }
    const events = await engine.query({ kind: 'queryBook', symbol, depth }, { timeoutMs: requestTimeoutMs });
    const level = (e) => ({ price: paiseToRupees(e.price), quantity: e.qty });
    res.json({
      symbol: symbolName(symbol),
      bids: events.filter((e) => e.type === EventType.BOOK_LEVEL && e.side === Side.BUY).map(level),
      asks: events.filter((e) => e.type === EventType.BOOK_LEVEL && e.side === Side.SELL).map(level),
    });
  }));

  app.use((req, res) => {
    res.status(404).json({ error: 'NOT_FOUND' });
  });

  // eslint-disable-next-line no-unused-vars
  app.use((err, req, res, next) => {
    if (err.type === 'entity.parse.failed') {
      res.status(400).json({ error: 'INVALID_JSON', message: 'the body is not valid JSON' });
      return;
    }
    if (err.type === 'entity.too.large') {
      res.status(413).json({ error: 'BODY_TOO_LARGE' });
      return;
    }
    switch (err.code) {
      case 'QUERY_REJECTED': {
        const reason = nameOf(RejectReason, err.reason);
        res.status(reason === 'UNKNOWN_USER' ? 403 : 404).json({
          error: reason,
          message: REJECT_TEXT[reason] ?? 'the engine refused the query',
        });
        return;
      }
      case 'ENGINE_UNAVAILABLE':
        res.status(503).json({ error: 'ENGINE_UNAVAILABLE', message: 'the trading engine is not reachable' });
        return;
      case 'ETIMEDOUT':
        res.status(504).json({ error: 'ENGINE_TIMEOUT', message: 'no reply from the engine; the request may or may not have been applied' });
        return;
      case 'ECONNLOST':
        res.status(502).json({ error: 'ENGINE_CONNECTION_LOST', message: 'the connection to the engine dropped; the request may or may not have been applied' });
        return;
      default:
        console.error('unexpected error:', err);
        res.status(500).json({ error: 'INTERNAL_ERROR' });
    }
  });

  return app;
}