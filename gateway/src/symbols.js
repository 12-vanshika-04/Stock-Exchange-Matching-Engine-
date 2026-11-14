// Engine symbol ids 0..5 in the order of the project synopsis.
export const SYMBOLS = Object.freeze(['RELIANCE', 'TCS', 'INFY', 'HDFCBANK', 'ICICIBANK', 'SBIN']);

/** @returns {number} the engine symbol id, or -1 if the name is unknown */
export const symbolId = (name) => SYMBOLS.indexOf(String(name).toUpperCase());

/** @returns {string|undefined} */
export const symbolName = (id) => SYMBOLS[id];