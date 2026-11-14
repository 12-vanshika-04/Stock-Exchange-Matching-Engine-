import { EngineClient } from '../src/engineClient.js';
import { EventType, OrderType, RejectReason, Side } from '../src/protocol.js';

const port = Number(process.argv[2] ?? 9000);
const REF = 250000; // RELIANCE's reference price in paise (Rs 2,500); must match engine_server

const client = new EngineClient({ port });
client.on('problem', (err) => console.error('problem:', err.message));

let failures = 0;
function check(name, ok, detail = '') {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!ok) failures++;
}
async function rejection(promise) {
  try {
    await promise;
    return null;
  } catch (err) {
    return err;
  }
}

const limit = (user, side, price, qty) => ({ kind: 'new', side, orderType: OrderType.LIMIT, symbol: 0, user, price, qty });
const levels = (events) => events.filter((e) => e.type === EventType.BOOK_LEVEL);
const bids = (events) => levels(events).filter((e) => e.side === Side.BUY);
const asks = (events) => levels(events).filter((e) => e.side === Side.SELL);

try {
  await client.start({ timeoutMs: 5000 });
  console.log(`connected to the engine on port ${port}\n`);

  // 1. A fresh account.
  const fresh = await client.query({ kind: 'queryAccount', user: 5 });
  check('the balance comes first: Rs 1 crore, nothing reserved',
    fresh[0].type === EventType.ACCOUNT_BALANCE && fresh[0].price === 1_000_000_000 && fresh[0].qty === 0,
    `cash ${fresh[0].price}, reserved ${fresh[0].qty}`);
  const positions = fresh.filter((e) => e.type === EventType.ACCOUNT_POSITION);
  check('one position per symbol, in symbol order', positions.length === 6 && positions.every((p, i) => p.symbol === i));
  check('every position is 1000 shares, none reserved, at the seeded cost',
    positions.every((p) => p.qty === 1000 && p.remaining === 0) && positions[0].price === 1000 * REF,
    `RELIANCE cost basis ${positions[0].price}`);

  // 2. Open orders become reserved cash, reserved shares and book levels.
  const orders = [];
  for (const [price, qty] of [[REF - 5000, 10], [REF - 4000, 5], [REF - 3000, 1]]) {
    orders.push({ user: 5, ack: await client.request(limit(5, Side.BUY, price, qty)) });
  }
  for (const [price, qty] of [[REF + 3000, 4], [REF + 2000, 2]]) {
    orders.push({ user: 6, ack: await client.request(limit(6, Side.SELL, price, qty)) });
  }
  const buyer = await client.query({ kind: 'queryAccount', user: 5 });
  check('open buys reserve cash', buyer[0].qty === 245000 * 10 + 246000 * 5 + 247000, `reserved ${buyer[0].qty}`);
  const seller = await client.query({ kind: 'queryAccount', user: 6 });
  const sellerReliance = seller.find((e) => e.type === EventType.ACCOUNT_POSITION && e.symbol === 0);
  check('open sells reserve shares', sellerReliance.qty === 1000 && sellerReliance.remaining === 6, `reserved ${sellerReliance.remaining}`);

  const book = await client.query({ kind: 'queryBook', symbol: 0, depth: 10 });
  check('bids come first and best (highest) first',
    JSON.stringify(bids(book).map((e) => [e.price, e.qty])) === JSON.stringify([[247000, 1], [246000, 5], [245000, 10]]));
  check('asks follow, best (lowest) first',
    JSON.stringify(asks(book).map((e) => [e.price, e.qty])) === JSON.stringify([[252000, 2], [253000, 4]]));
  check('all bids precede all asks', levels(book).findIndex((e) => e.side === Side.SELL) === 3);
  const shallow = await client.query({ kind: 'queryBook', symbol: 0, depth: 2 });
  check('depth 2 gives the best two levels of each side',
    bids(shallow).length === 2 && asks(shallow).length === 2 && bids(shallow)[0].price === 247000 && asks(shallow)[0].price === 252000);

  // 3. Cancelling everything empties the book and releases the reservations.
  for (const { user, ack } of orders) {
    await client.request({ kind: 'cancel', symbol: 0, user, orderId: ack.orderId });
  }
  const empty = await client.query({ kind: 'queryBook', symbol: 0, depth: 10 });
  check('the book is empty again', levels(empty).length === 0);
  const released = await client.query({ kind: 'queryAccount', user: 5 });
  check('the reservation was released', released[0].qty === 0);

  // 4. Refusals.
  const noUser = await rejection(client.query({ kind: 'queryAccount', user: 99999 }));
  check('an unknown account is refused', noUser?.code === 'QUERY_REJECTED' && noUser.reason === RejectReason.UNKNOWN_USER);
  const noSymbol = await rejection(client.query({ kind: 'queryBook', symbol: 9, depth: 5 }));
  check('an unknown symbol is refused', noSymbol?.code === 'QUERY_REJECTED' && noSymbol.reason === RejectReason.UNKNOWN_SYMBOL);
} catch (err) {
  console.error('\nsmoke test aborted:', err.message);
  failures++;
} finally {
  await client.stop();
}

console.log(failures === 0 ? '\nAll checks passed: the Node client and the C++ engine agree on queries.' : `\n${failures} check(s) FAILED`);
process.exitCode = failures === 0 ? 0 : 1;