import { ArrowDown, ArrowUp, Minus } from 'lucide-react';
import { priceMove } from '../lib/trades.js';

const time = (ms) => new Date(ms).toLocaleTimeString('en-IN', { hour12: false });

function Move({ move }) {
  if (move > 0) return <ArrowUp size={14} className="up" aria-label="up" />;
  if (move < 0) return <ArrowDown size={14} className="down" aria-label="down" />;
  return <Minus size={14} className="flat" aria-label="unchanged" />;
}

export default function TradeTape({ trades }) {
  if (trades.length === 0) return <p className="muted">No trades yet for this symbol.</p>;
  return (
    <div className="table-wrap">
      <table className="tape">
        <thead>
          <tr>
            <th>Time</th>
            <th className="num">Price (₹)</th>
            <th className="num">Qty</th>
          </tr>
        </thead>
        <tbody>
          {trades.map((t, i) => (
            <tr key={t.tradeId}>
              <td>{time(t.time)}</td>
              <td className="num">
                <Move move={priceMove(trades, i)} /> {t.price}
              </td>
              <td className="num">{t.quantity}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}