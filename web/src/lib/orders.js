export const MAX_FILLS = 100;

export const initialOrders = () => ({ byId: {}, ids: [], fills: [] });

/**
 * The account's orders and fills for this session, built from the gateway's private events.
 *
 *   accepted  {order}    an order as the gateway reports it (REST reply or socket 'order' event)
 *   fill      {fill}     one execution of one of this account's orders
 *   cancelled {orderId}
 *   clear
 *
 * An accepted order already includes the fills it got on the spot, and those fills follow as
 * separate events. They must not be counted twice, so describeOrder uses
 * max(filled at acceptance, sum of fills), which is right whichever arrives first.
 */
export function ordersReducer(state, action) {
  switch (action.type) {
    case 'accepted': {
      const o = action.order;
      const existing = state.byId[o.orderId];
      const record = {
        orderId: o.orderId,
        symbol: o.symbol,
        side: o.side,
        price: o.price,
        quantity: o.quantity,
        executedAtAcceptance: o.quantity - o.remaining,
        cancelled: o.status === 'CANCELLED' || (existing?.cancelled ?? false),
        fills: existing?.fills ?? {},
      };
      return {
        ...state,
        byId: { ...state.byId, [o.orderId]: record },
        ids: existing ? state.ids : [o.orderId, ...state.ids],
      };
    }
    case 'fill': {
      const f = action.fill;
      const key = `${f.orderId}:${f.tradeId}`;
      if (state.fills.some((x) => x.key === key)) return state; // already counted
      const fills = [{ ...f, key }, ...state.fills].slice(0, MAX_FILLS);
      const record = state.byId[f.orderId];
      if (!record) return { ...state, fills }; // an order placed before this page loaded
      return {
        ...state,
        fills,
        byId: { ...state.byId, [f.orderId]: { ...record, fills: { ...record.fills, [f.tradeId]: f.quantity } } },
      };
    }
    case 'cancelled': {
      const record = state.byId[action.orderId];
      if (!record || record.cancelled) return state;
      return { ...state, byId: { ...state.byId, [action.orderId]: { ...record, cancelled: true } } };
    }
    case 'clear':
      return initialOrders();
    default:
      return state;
  }
}

/** Adds what the UI shows: filled, remaining, status and whether the order can still be cancelled. */
export function describeOrder(record) {
  const fillSum = Object.values(record.fills).reduce((a, b) => a + b, 0);
  const filled = Math.min(record.quantity, Math.max(record.executedAtAcceptance, fillSum));
  let status;
  if (record.cancelled) status = 'CANCELLED';
  else if (filled >= record.quantity) status = 'FILLED';
  else if (filled > 0) status = 'PARTIAL';
  else status = 'NEW';
  return { ...record, filled, remaining: record.quantity - filled, status, open: status === 'NEW' || status === 'PARTIAL' };
}

/** Every order of the session, newest first. */
export const selectOrders = (state) => state.ids.map((id) => describeOrder(state.byId[id]));