import { useMemo, useState } from 'react';
import { TrendingUp } from 'lucide-react';
import AccountPicker from './components/AccountPicker.jsx';
import MyFills from './components/MyFills.jsx';
import MyOrders from './components/MyOrders.jsx';
import OrderBook from './components/OrderBook.jsx';
import OrderForm from './components/OrderForm.jsx';
import Portfolio from './components/Portfolio.jsx';
import StatusBadge from './components/StatusBadge.jsx';
import SymbolTabs from './components/SymbolTabs.jsx';
import TradeTape from './components/TradeTape.jsx';
import { useGateway } from './hooks/useGateway.js';
import { useResource } from './hooks/useResource.js';
import { getAccount, getBook } from './lib/api.js';
import { selectOrders } from './lib/orders.js';
import { SYMBOLS } from './lib/symbols.js';
import { priceMove } from './lib/trades.js';

const BOOK_DEPTH = 10;

function loadAccount() {
  try {
    const saved = Number(localStorage.getItem('devAccount'));
    return Number.isInteger(saved) && saved >= 0 ? saved : 0;
  } catch {
    return 0; // storage can be blocked
  }
}

export default function App() {
  const [account, setAccount] = useState(loadAccount);
  const [symbol, setSymbol] = useState(SYMBOLS[0]);
  const { status, trades, orders, gap, recordOrder, recordCancel } = useGateway(account);
  const myOrders = useMemo(() => selectOrders(orders), [orders]);

  const tape = trades[symbol];

  // The account reloads when something happens to it: `orders` changes on every one of its
  // orders, fills and cancels, and on an account switch.
  const accountInfo = useResource((signal) => getAccount(account, signal), [account, orders], { key: account });

  // The book reloads on this symbol's trades (`tape` changes) and on this account's activity,
  // and polls once a second: another account's resting order moves the book without a trade.
  const bookInfo = useResource((signal) => getBook(symbol, BOOK_DEPTH, signal), [symbol, tape, orders], {
    key: symbol,
    pollMs: 1000,
  });

  function switchAccount(next) {
    try {
      localStorage.setItem('devAccount', String(next));
    } catch {
      /* not important */
    }
    setAccount(next);
  }

  const last = tape[0];
  const move = priceMove(tape, 0);

  return (
    <div className="app">
      <header className="header">
        <div className="brand">
          <TrendingUp size={22} aria-hidden="true" />
          <span>Exchange</span>
        </div>
        <StatusBadge status={status} />
        <AccountPicker key={account} account={account} onChange={switchAccount} />
      </header>

      <SymbolTabs symbols={SYMBOLS} selected={symbol} onSelect={setSymbol} trades={trades} />

      <div className="layout">
        <section className="card summary area-summary" aria-live="polite">
          <div className="summary-symbol">{symbol}</div>
          <div className={move > 0 ? 'summary-price up' : move < 0 ? 'summary-price down' : 'summary-price'}>
            {last ? `₹${last.price}` : '—'}
          </div>
          <div className="muted">
            {tape.length} recent trade{tape.length === 1 ? '' : 's'}
          </div>
        </section>

        <section className="card area-form">
          <h2>Place an order</h2>
          <OrderForm key={`${account}:${symbol}`} account={account} symbol={symbol} onPlaced={recordOrder} />
        </section>

        <section className="card area-book">
          <h2>Order book · {symbol}</h2>
          <OrderBook info={bookInfo} symbol={symbol} />
        </section>

        <section className="card area-portfolio">
          <h2>Portfolio</h2>
          <Portfolio info={accountInfo} symbol={symbol} />
        </section>

        <section className="card area-orders">
          <h2>My orders</h2>
          <MyOrders account={account} orders={myOrders} gap={gap} onCancelled={recordCancel} />
        </section>

        <section className="card area-fills">
          <h2>My fills</h2>
          <MyFills fills={orders.fills} />
        </section>

        <section className="card area-tape">
          <h2>Recent trades</h2>
          <TradeTape trades={tape} />
        </section>
      </div>
    </div>
  );
}