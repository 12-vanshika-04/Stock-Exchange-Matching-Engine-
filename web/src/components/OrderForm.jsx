import { useState } from 'react';
import { RefreshCw, TrendingDown, TrendingUp } from 'lucide-react';
import { placeOrder } from '../lib/api.js';
import { validateOrder } from '../lib/orderForm.js';

function describeAck(order) {
  const filled = order.quantity - order.remaining;
  switch (order.status) {
    case 'FILLED':
      return `Order ${order.orderId} filled (${order.quantity} shares).`;
    case 'PARTIAL':
      return `Order ${order.orderId}: ${filled} of ${order.quantity} filled, the rest is waiting in the book.`;
    case 'CANCELLED':
      return filled > 0
        ? `Order ${order.orderId}: ${filled} of ${order.quantity} filled, the rest was cancelled because nothing more matched.`
        : `Order ${order.orderId} was cancelled: nothing was available to match.`;
    default:
      return `Order ${order.orderId} is waiting in the book.`;
  }
}

export default function OrderForm({ account, symbol, onPlaced }) {
  const [side, setSide] = useState('buy');
  const [type, setType] = useState('limit');
  const [price, setPrice] = useState('');
  const [quantity, setQuantity] = useState('');
  const [errors, setErrors] = useState({});
  const [pending, setPending] = useState(false);
  const [result, setResult] = useState(null);

  async function submit(event) {
    event.preventDefault();
    if (pending) return;
    const check = validateOrder({ side, type, price, quantity });
    setErrors(check.ok ? {} : check.errors);
    setResult(null);
    if (!check.ok) return;

    setPending(true);
    try {
      const order = await placeOrder(account, { symbol, ...check.order });
      onPlaced(order);
      setResult({ ok: true, text: describeAck(order) });
    } catch (err) {
      setResult({ ok: false, text: err.message });
    } finally {
      setPending(false);
    }
  }

  return (
    <form className="form" onSubmit={submit} noValidate>
      <div className="seg" role="group" aria-label="Side">
        <button type="button" className="buy" aria-pressed={side === 'buy'} onClick={() => setSide('buy')}>
          <TrendingUp size={16} aria-hidden="true" /> Buy
        </button>
        <button type="button" className="sell" aria-pressed={side === 'sell'} onClick={() => setSide('sell')}>
          <TrendingDown size={16} aria-hidden="true" /> Sell
        </button>
      </div>

      <div className="seg" role="group" aria-label="Order type">
        <button type="button" aria-pressed={type === 'limit'} onClick={() => setType('limit')}>
          Limit
        </button>
        <button type="button" aria-pressed={type === 'market'} onClick={() => setType('market')}>
          Market
        </button>
      </div>

      {type === 'limit' ? (
        <div className="field">
          <label htmlFor="price">Price (₹)</label>
          <input
            id="price"
            inputMode="decimal"
            placeholder="e.g. 2500.05"
            value={price}
            onChange={(e) => setPrice(e.target.value)}
            aria-invalid={Boolean(errors.price)}
          />
          {errors.price ? <span className="field-error">{errors.price}</span> : <span className="hint">Prices move in steps of ₹0.05.</span>}
        </div>
      ) : (
        <p className="hint">A market order trades at the best prices available right now.</p>
      )}

      <div className="field">
        <label htmlFor="quantity">Quantity</label>
        <input
          id="quantity"
          inputMode="numeric"
          placeholder="shares"
          value={quantity}
          onChange={(e) => setQuantity(e.target.value)}
          aria-invalid={Boolean(errors.quantity)}
        />
        {errors.quantity && <span className="field-error">{errors.quantity}</span>}
      </div>

      <button type="submit" className={`submit ${side}`} disabled={pending}>
        {pending && <RefreshCw size={16} className="spin" aria-hidden="true" />}
        {side === 'buy' ? 'Buy' : 'Sell'} {symbol}
      </button>

      {result && (
        <div className={result.ok ? 'result ok' : 'result error'} role="status">
          {result.text}
        </div>
      )}
    </form>
  );
}