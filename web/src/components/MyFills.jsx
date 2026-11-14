const time = (ms) => new Date(ms).toLocaleTimeString('en-IN', { hour12: false });

export default function MyFills({ fills }) {
  if (fills.length === 0) return <p className="muted">Your executions appear here as they happen.</p>;
  return (
    <div className="table-wrap">
      <table className="tape">
        <thead>
          <tr>
            <th>Time</th>
            <th>Symbol</th>
            <th>Side</th>
            <th className="num">Price (₹)</th>
            <th className="num">Qty</th>
          </tr>
        </thead>
        <tbody>
          {fills.map((f) => (
            <tr key={f.key}>
              <td>{time(f.time)}</td>
              <td>{f.symbol}</td>
              <td className={f.side === 'buy' ? 'side-buy' : 'side-sell'}>{f.side}</td>
              <td className="num">{f.price}</td>
              <td className="num">{f.quantity}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}