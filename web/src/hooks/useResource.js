import { useEffect, useState } from 'react';

/**
 * Loads data with load(signal) and loads it again whenever `deps` change (after a short
 * pause, so a burst of changes causes one request) and, if pollMs > 0, every pollMs while
 * the page is visible. `key` says what the data is about (an account, a symbol): data
 * loaded for another key is never returned. On an error the last good data is kept.
 * Returns { data, error, loading }.
 */
export function useResource(load, deps, { key, pollMs = 0, debounceMs = 100 } = {}) {
  const [state, setState] = useState({ key: undefined, data: null, error: null });

  useEffect(() => {
    let cancelled = false;
    const controller = new AbortController();

    async function run() {
      try {
        const data = await load(controller.signal);
        if (!cancelled) setState({ key, data, error: null });
      } catch (err) {
        if (cancelled || err.name === 'AbortError') return;
        setState((prev) => ({ key, data: prev.key === key ? prev.data : null, error: err }));
      }
    }

    const timer = setTimeout(run, debounceMs);
    const poll =
      pollMs > 0
        ? setInterval(() => {
            if (document.visibilityState === 'visible') run();
          }, pollMs)
        : null;

    return () => {
      cancelled = true;
      controller.abort(); // an old request must not overwrite newer data
      clearTimeout(timer);
      if (poll) clearInterval(poll);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);

  const current = state.key === key;
  return { data: current ? state.data : null, error: current ? state.error : null, loading: !current };
}