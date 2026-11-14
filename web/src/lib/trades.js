export const MAX_TRADES = 100;

export const emptyTrades = (symbols) => Object.fromEntries(symbols.map((s) => [s, []]));

/** "2500.05" -> 250005 paise. The gateway sends prices as exact decimal strings. */
export function paiseOf(price) {
  const [whole, frac = ''] = String(price).split('.');
  return Number(whole) * 100 + Number((frac + '00').slice(0, 2));
}

/**
 * Direction of the trade at `index` compared with the one before it in time (the next entry,
 * because the list is newest first): 1 up, -1 down, 0 unchanged or no earlier trade.
 */
export function priceMove(trades, index = 0) {
  const now = trades[index];
  const before = trades[index + 1];
  if (!now || !before) return 0;
  return Math.sign(paiseOf(now.price) - paiseOf(before.price));
}

/**
 * State: { SYMBOL: [trade, ...] } with the newest trade first.
 *   snapshot  {recent: {SYMBOL: [oldest ... newest]}}  replaces the lists (on every connect)
 *   trade     {trade}                                   adds one live trade
 *   clear                                               empties everything
 */
export function tradesReducer(state, action) {
  switch (action.type) {
    case 'snapshot': {
      const next = { ...state };
      for (const [symbol, list] of Object.entries(action.recent)) {
        if (symbol in next) next[symbol] = list.slice().reverse().slice(0, MAX_TRADES);
      }
      return next;
    }
    case 'trade': {
      const { trade } = action;
      const list = state[trade.symbol];
      if (!list) return state; // a symbol this page does not know
      if (list.length > 0 && list[0].tradeId === trade.tradeId) return state; // never show a trade twice
      return { ...state, [trade.symbol]: [trade, ...list].slice(0, MAX_TRADES) };
    }
    case 'clear':
      return emptyTrades(Object.keys(state));
    default:
      return state;
  }
}