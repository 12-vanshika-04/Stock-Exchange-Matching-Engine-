// REST calls to the gateway. The development login sends the account number as a header.
export class ApiError extends Error {
  constructor(status, body) {
    super(body?.message ?? body?.error ?? `request failed (HTTP ${status})`);
    this.name = 'ApiError';
    this.status = status;
    this.code = body?.error ?? null;
  }
}

// account === null sends no login header (public market data).
async function call(method, path, account, body, signal) {
  const headers = {};
  if (account !== null) headers['x-user-id'] = String(account);
  if (body !== undefined) headers['content-type'] = 'application/json';
  let res;
  try {
    res = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal,
    });
  } catch (err) {
    if (err.name === 'AbortError') throw err; // a request we cancelled ourselves
    throw new ApiError(0, { error: 'NETWORK', message: 'could not reach the gateway' });
  }
  let json = null;
  try {
    json = await res.json();
  } catch {
    /* the body was not JSON */
  }
  if (!res.ok) throw new ApiError(res.status, json);
  return json;
}

/** order: { symbol, side: 'buy'|'sell', type: 'limit'|'market', price?: '2500.05', quantity: 10 } */
export const placeOrder = (account, order) => call('POST', '/api/orders', account, order);

export const cancelOrder = (account, orderId) => call('DELETE', `/api/orders/${orderId}`, account);

/** { cash, reservedCash, availableCash, positions: [{ symbol, quantity, reserved, available, averageCost, costBasis }] } */
export const getAccount = (account, signal) => call('GET', '/api/account', account, undefined, signal);

/** { symbol, bids: [{ price, quantity }], asks: [...] }, best price first. Public: no login. */
export const getBook = (symbol, depth, signal) =>
  call('GET', `/api/book/${symbol}?depth=${depth}`, null, undefined, signal);