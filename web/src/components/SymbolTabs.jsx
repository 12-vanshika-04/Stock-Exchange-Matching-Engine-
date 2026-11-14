export default function SymbolTabs({ symbols, selected, onSelect, trades }) {
  return (
    <div className="tabs" role="tablist" aria-label="Symbols">
      {symbols.map((symbol) => {
        const last = trades[symbol][0];
        const active = symbol === selected;
        return (
          <button
            key={symbol}
            role="tab"
            aria-selected={active}
            className={active ? 'tab active' : 'tab'}
            onClick={() => onSelect(symbol)}
          >
            <span className="tab-name">{symbol}</span>
            <span className="tab-price">{last ? `₹${last.price}` : '—'}</span>
          </button>
        );
      })}
    </div>
  );
}