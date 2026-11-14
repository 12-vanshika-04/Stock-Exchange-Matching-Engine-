import { formatRupees } from '../lib/format.js';

function Stat({ label, value }) {
  return (
    <div className="stat">
      <span className="stat-label">{label}</span>
      <span className="stat-value">₹{formatRupees(value)}</span>
    </div>
  );
}

export default function Portfolio({ info, symbol }) {
  const { data, error } = info;
  if (!data) return <p className="muted">{error ? error.message : 'Loading…'}</p>;
  return (
    <>
      <div className="stats">
        <Stat label="Cash" value={data.cash} />
        <Stat label="Reserved" value={data.reservedCash} />
        <Stat label="Available" value={data.availableCash} />
      </div>
      <div className="table-wrap">
        <table className="tape">
          <thead>
            <tr>
              <th>Symbol</th>
              <th className="num">Shares</th>
              <th className="num">Reserved</th>
              <th className="num">Avg cost (₹)</th>
            </tr>
          </thead>
          <tbody>
            {data.positions.map((p) => (
              <tr key={p.symbol} className={p.symbol === symbol ? 'row-selected' : undefined}>
                <td>{p.symbol}</td>
                <td className="num">{p.quantity}</td>
                <td className="num">{p.reserved}</td>
                <td className="num">{p.quantity > 0 ? formatRupees(p.averageCost) : '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {error && <div className="result error">{error.message}</div>}
    </>
  );
}