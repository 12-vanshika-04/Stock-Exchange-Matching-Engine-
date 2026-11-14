import { bookView } from '../lib/book.js';

function Levels({ levels, side }) {
  return levels.map((l) => (
    <div key={`${side}-${l.price}`} className={`level level-${side}`} style={{ '--w': `${l.pct}%` }}>
      <span>{l.price}</span>
      <span>{l.quantity}</span>
    </div>
  ));
}

export default function OrderBook({ info, symbol }) {
  const { data, error } = info;
  if (!data) return <p className="muted">{error ? error.message : 'Loading…'}</p>;
  const view = bookView(data);
  const empty = view.asks.length === 0 && view.bids.length === 0;
  return (
    <>
      {empty ? (
        <p className="muted">No resting orders for {symbol}.</p>
      ) : (
        <div className="book">
          <div className="book-head">
            <span>Price (₹)</span>
            <span>Quantity</span>
          </div>
          <Levels levels={view.asks} side="ask" />
          <div className="spread">{view.spread ? `Spread ₹${view.spread}` : '—'}</div>
          <Levels levels={view.bids} side="bid" />
        </div>
      )}
      {error && <div className="result error">{error.message}</div>}
    </>
  );
}