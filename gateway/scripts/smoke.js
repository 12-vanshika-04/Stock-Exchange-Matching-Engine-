import { EngineClient } from '../src/engineClient.js';
import {
  EventType,
  OrderStatus,
  OrderType,
  RejectReason,
  Side,
  localOrderId,
  symbolOfOrderId,
} from '../src/protocol.js';

const port = Number(process.argv[2] ?? 9000);
// RELIANCE's reference price in paise (Rs 2,500). Must match engine_server: the band is +-20%.
const REF = 250000;

const client = new EngineClient({ port });
const events = [];
client.on('event', (e) => events.push(e));
client.on('problem', (err) => console.error('problem:', err.message));

let failures = 0;
function check(name, ok, detail = '') {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!ok) failures++;
}

async function waitFor(condition, ms = 2000) {
  const end = Date.now() + ms;
  while (!condition()) {
    if (Date.now() > end) return false;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  return true;
}

const limit = (user, side, price, qty) => ({
  kind: 'new', side, orderType: OrderType.LIMIT, symbol: 0, user, price, qty,
});

try {
  await client.start({ timeoutMs: 5000 });
  console.log(`connected to the engine on port ${port}\n`);

  // 1. A resting sell, then a matching buy: one real trade.
  const sell = await client.request(limit(0, Side.SELL, REF, 10));
  check('sell order accepted and resting',
    sell.type === EventType.ORDER_ACCEPTED && sell.status === OrderStatus.NEW &&
    sell.remaining === 10 && sell.qty === 10 && sell.price === REF &&
    sell.user === 0 && sell.side === Side.SELL,
    `status ${sell.status}, remaining ${sell.remaining}`);
  check('order id carries symbol 0 and a local number',
    symbolOfOrderId(sell.orderId) === 0 && localOrderId(sell.orderId) >= 1n,
    `orderId ${sell.orderId}`);

  const buy = await client.request(limit(1, Side.BUY, REF, 10));
  check('buy order filled at once',
    buy.type === EventType.ORDER_ACCEPTED && buy.status === OrderStatus.FILLED && buy.remaining === 0,
    `status ${buy.status}, remaining ${buy.remaining}`);

  const gotTrade = await waitFor(() => events.some((e) => e.type === EventType.TRADE && e.clientRef === buy.clientRef));
  const t = events.find((e) => e.type === EventType.TRADE && e.clientRef === buy.clientRef)?.trade;
  check('a trade event follows', gotTrade);
  check('trade price, quantity, buyer and seller are right',
    t !== undefined && t.price === REF && t.qty === 10 && t.buyerId === 1 && t.sellerId === 0 && t.symbol === 0,
    t ? `price ${t.price}, qty ${t.qty}, buyer ${t.buyerId}, seller ${t.sellerId}` : '');
  check('trade points at the right orders',
    t !== undefined && t.buyOrderId === buy.orderId && t.sellOrderId === sell.orderId);

  // 2. Rejections: the reason codes must match the engine's.
  const unknown = await client.request(limit(99999, Side.BUY, REF, 1));
  check('unknown user is rejected', unknown.type === EventType.ORDER_REJECTED && unknown.reason === RejectReason.UNKNOWN_USER && unknown.status === OrderStatus.REJECTED);
  const offTick = await client.request(limit(0, Side.BUY, REF + 3, 1));
  check('price off the tick is rejected', offTick.type === EventType.ORDER_REJECTED && offTick.reason === RejectReason.PRICE_OFF_TICK);
  const outBand = await client.request(limit(0, Side.BUY, 300005, 1)); // just above Rs 3,000
  check('price outside the band is rejected', outBand.type === EventType.ORDER_REJECTED && outBand.reason === RejectReason.PRICE_OUT_OF_BAND);
  const oldBand = await client.request(limit(0, Side.BUY, 50000, 1)); // Rs 500: fine under the old shared price, far below Rs 2,000 now
  check('the old shared Rs 500 price no longer works for RELIANCE', oldBand.type === EventType.ORDER_REJECTED && oldBand.reason === RejectReason.PRICE_OUT_OF_BAND);

  // 3. Cancel: wrong owner, right owner, and a repeat.
  const rest = await client.request(limit(2, Side.BUY, REF - 5000, 5)); // Rs 2,450: inside the band, far from the market
  check('a buy far from the market rests', rest.type === EventType.ORDER_ACCEPTED && rest.status === OrderStatus.NEW);
  const cancelCmd = (user) => ({ kind: 'cancel', symbol: 0, user, orderId: rest.orderId });
  const wrongOwner = await client.request(cancelCmd(3));
  check('another user cannot cancel it', wrongOwner.type === EventType.CANCEL_REJECTED);
  const cancelled = await client.request(cancelCmd(2));
  check('the owner can cancel it', cancelled.type === EventType.ORDER_CANCELLED && cancelled.orderId === rest.orderId);
  const again = await client.request(cancelCmd(2));
  check('cancelling twice is refused', again.type === EventType.CANCEL_REJECTED);
} catch (err) {
  console.error('\nsmoke test aborted:', err.message);
  failures++;
} finally {
  await client.stop();
}

console.log(failures === 0 ? '\nAll checks passed: the Node client and the C++ engine agree.' : `\n${failures} check(s) FAILED`);
process.exitCode = failures === 0 ? 0 : 1;