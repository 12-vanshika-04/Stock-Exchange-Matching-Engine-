import { formatPaise } from './format.js';
import { paiseOf } from './trades.js';

/**
 * Turns the gateway's { bids, asks } (both best price first) into what the page draws:
 *   asks    highest price first, so the best ask sits next to the spread
 *   bids    best (highest) first
 *   spread  best ask minus best bid as an exact rupee string, or null if a side is empty
 * Every level gets `pct`, the width of its depth bar: its quantity as a share of the
 * largest quantity on either side (never under 2, so a small level stays visible).
 */
export function bookView(book) {
  const maxQty = [...book.bids, ...book.asks].reduce((m, l) => Math.max(m, l.quantity), 0);
  const withPct = (l) => ({
    ...l,
    pct: maxQty > 0 ? Math.max(2, Math.round((l.quantity / maxQty) * 100)) : 0,
  });
  const spread =
    book.bids.length > 0 && book.asks.length > 0
      ? formatPaise(paiseOf(book.asks[0].price) - paiseOf(book.bids[0].price))
      : null;
  return {
    asks: book.asks.map(withPct).reverse(),
    bids: book.bids.map(withPct),
    spread,
  };
}