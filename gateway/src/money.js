/**
 * "500.05" or 500.05 -> 50005 paise. Returns null unless the input is a plain
 * non-negative amount with at most two decimals. Parsing is done on the text, so no
 * floating-point arithmetic is involved.
 */
export function rupeesToPaise(input) {
  const s = typeof input === 'number' ? String(input) : input;
  if (typeof s !== 'string' || !/^\d{1,9}(\.\d{1,2})?$/.test(s)) return null;
  const [whole, frac = ''] = s.split('.');
  return Number(whole) * 100 + Number((frac + '00').slice(0, 2));
}

/** 50005 -> "500.05" */
export function paiseToRupees(paise) {
  const sign = paise < 0 ? '-' : '';
  const abs = Math.abs(paise);
  return `${sign}${Math.floor(abs / 100)}.${String(abs % 100).padStart(2, '0')}`;
}