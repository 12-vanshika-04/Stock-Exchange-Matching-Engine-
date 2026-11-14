/** "1234567.5" -> "12,34,567.5": digits grouped the Indian way (last three, then pairs). */
export function formatRupees(amount) {
  const [whole, frac] = String(amount).split('.');
  const sign = whole.startsWith('-') ? '-' : '';
  const digits = sign ? whole.slice(1) : whole;
  const last3 = digits.slice(-3);
  const rest = digits.slice(0, -3);
  const grouped = rest ? `${rest.replace(/\B(?=(\d{2})+(?!\d))/g, ',')},${last3}` : last3;
  return `${sign}${grouped}${frac !== undefined ? `.${frac}` : ''}`;
}

/** 250005 paise -> "2500.05" */
export function formatPaise(paise) {
  const sign = paise < 0 ? '-' : '';
  const abs = Math.abs(paise);
  return `${sign}${Math.floor(abs / 100)}.${String(abs % 100).padStart(2, '0')}`;
}