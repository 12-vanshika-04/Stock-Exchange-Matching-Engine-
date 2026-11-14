import { OrderStatus, Side } from './protocol.js';
import { paiseToRupees } from './money.js';
import { symbolName } from './symbols.js';

export const nameOf = (table, value) =>
  Object.entries(table).find(([, v]) => v === value)?.[0] ?? String(value);

/** The JSON shape of an order, as sent to browsers. IDs are strings: JSON cannot carry a bigint. */
export function orderJson(e) {
  return {
    orderId: String(e.orderId),
    symbol: symbolName(e.symbol) ?? String(e.symbol),
    side: e.side === Side.BUY ? 'buy' : 'sell',
    price: paiseToRupees(e.price),
    quantity: e.qty,
    remaining: e.remaining,
    status: nameOf(OrderStatus, e.status),
  };
}