import { paiseOf } from './trades.js';

export const MAX_QTY = 100_000_000; // the same limit the gateway and the engine enforce

const QTY = /^[1-9]\d{0,8}$/;
const PRICE = /^\d{1,9}(\.\d{1,2})?$/;

/**
 * Checks the form's text fields and builds the request body.
 * Returns { ok: true, order } or { ok: false, errors: { price?, quantity? } }.
 * Only the format is checked here. The price band, the tick size and the account's funds
 * are the engine's rules, and its answer is shown as it comes back.
 */
export function validateOrder({ side, type, price, quantity }) {
  const errors = {};

  const q = String(quantity).trim();
  let qty = null;
  if (!QTY.test(q) || Number(q) > MAX_QTY) errors.quantity = `Enter a whole number from 1 to ${MAX_QTY}`;
  else qty = Number(q);

  let limitPrice = null;
  if (type === 'limit') {
    const p = String(price).trim();
    if (!PRICE.test(p) || paiseOf(p) <= 0) errors.price = 'Enter an amount like 2500.05 (at most two decimals)';
    else limitPrice = p;
  }

  if (Object.keys(errors).length > 0) return { ok: false, errors };
  return {
    ok: true,
    order: type === 'limit' ? { side, type, price: limitPrice, quantity: qty } : { side, type, quantity: qty },
  };
}