import { useState } from 'react';
import { Info, X } from 'lucide-react';
import { cancelOrder } from '../lib/api.js';

export default function MyOrders({ account, orders, gap, onCancelled }) {
  const [busy, setBusy] = useState(null); // the order being cancelled
  const [message, setMessage] = useState(null);

  async function cancel(orderId) {
    setBusy(orderId);
    setMessage(null);
    try {
      await cancelOrder(account, orderId);
      onCancelled(orderId);
    } catch (err) {
      setMessage(err.message);
    } finally {
      setBusy(null);
    }
  }

  return (
    <>
      {gap && (
        <p className="notice" role="status">
          <Info size={16} aria-hidden="true" /> The connection was interrupted, so some updates may be missing.
        </p>
      )}
      {orders.length === 0 ? (
        <p className="muted">Orders you place in this session appear here. The list starts empty after a page reload.</p>
      ) : (
        <div className="table-wrap">
          <table className="tape">
            <thead>
              <tr>
                <th>Symbol</th>
                <th>Side</th>
                <th className="num">Price (₹)</th>
                <th className="num">Filled</th>
                <th>Status</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {orders.map((o) => (
                <tr key={o.orderId}>
                  <td>{o.symbol}</td>
                  <td className={o.side === 'buy' ? 'side-buy' : 'side-sell'}>{o.side}</td>
                  <td className="num">{o.price === '0.00' ? 'market' : o.price}</td>
                  <td className="num">
                    {o.filled}/{o.quantity}
                  </td>
                  <td>
                    <span className={`status status-${o.status.toLowerCase()}`}>{o.status}</span>
                  </td>
                  <td>
                    {o.open && (
                      <button
                        type="button"
                        className="icon-btn"
                        aria-label={`Cancel order ${o.orderId}`}
                        disabled={busy === o.orderId}
                        onClick={() => cancel(o.orderId)}
                      >
                        <X size={14} aria-hidden="true" />
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {message && <div className="result error">{message}</div>}
    </>
  );
}