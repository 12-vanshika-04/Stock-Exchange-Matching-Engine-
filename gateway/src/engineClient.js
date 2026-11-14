import net from 'node:net';
import { EventEmitter } from 'node:events';
import { EventType, FrameReader, decodeEventFrame, encodeCommand } from './protocol.js';

// Event types that belong to the answer of a query (see EngineClient.query).
const QUERY_ANSWER = new Set([
  EventType.ACCOUNT_BALANCE,
  EventType.ACCOUNT_POSITION,
  EventType.BOOK_LEVEL,
  EventType.QUERY_DONE,
  EventType.QUERY_REJECTED,
]);

function engineError(message, code) {
  const err = new Error(message);
  err.code = code;
  return err;
}

/**
 * Talks to the C++ engine over TCP.
 *
 * Events: 'event' (every decoded engine event), 'connected', 'disconnected',
 *         'problem' (a socket error or a protocol violation; the client recovers by itself).
 *
 * Delivery across a reconnect: engine events can be repeated or lost, so the layer above
 * must tolerate both (dedupe on clientRef, rebuild state from the database).
 */
export class EngineClient extends EventEmitter {
  #host;
  #port;
  #reconnect;
  #minDelayMs;
  #maxDelayMs;
  #delayMs;
  #socket = null;
  #up = false;
  #stopped = false;
  #retryTimer = null;
  #startPromise = null;
  #startSettle = null;
  #pending = new Map(); // clientRef (bigint) -> { resolve, reject, timer }
  #queries = new Map(); // clientRef (bigint) -> { events, resolve, reject, timer }
  // Starts from the clock so refs stay unique across gateway restarts (and fit in a u64).
  #nextRef = BigInt(Date.now()) * 1_000_000n;

  /**
   * @param {{host?: string, port?: number, reconnect?: boolean,
   *          minDelayMs?: number, maxDelayMs?: number}} [options]
   */
  constructor({ host = '127.0.0.1', port = 9000, reconnect = true, minDelayMs = 100, maxDelayMs = 5000 } = {}) {
    super();
    this.#host = host;
    this.#port = port;
    this.#reconnect = reconnect;
    this.#minDelayMs = minDelayMs;
    this.#maxDelayMs = maxDelayMs;
    this.#delayMs = minDelayMs;
  }

  get connected() {
    return this.#up;
  }

  /**
   * Connects. Resolves on the first successful connection. With timeoutMs > 0 it rejects
   * if that takes too long (the client keeps retrying unless you call stop()).
   */
  start({ timeoutMs = 0 } = {}) {
    if (this.#startPromise) return this.#startPromise;
    this.#startPromise = new Promise((resolve, reject) => {
      this.#startSettle = { resolve, reject };
    });
    if (timeoutMs > 0) {
      const timer = setTimeout(
        () => this.#failStart(engineError(`could not connect to the engine within ${timeoutMs} ms`, 'ETIMEDOUT')),
        timeoutMs,
      );
      this.#startPromise.then(
        () => clearTimeout(timer),
        () => clearTimeout(timer),
      );
    }
    this.#connect();
    return this.#startPromise;
  }

  /**
   * Writes one command. Throws RangeError/TypeError for bad values and an error with
   * code 'ENGINE_UNAVAILABLE' when not connected. Returns false when the socket's write
   * buffer is full (wait for 'drain' on the socket if you are sending in bulk).
   */
  send(command) {
    if (!this.#up) throw engineError('not connected to the engine', 'ENGINE_UNAVAILABLE');
    return this.#socket.write(encodeCommand(command));
  }

  /**
   * Sends a command and resolves with its first non-trade event (for a new order: the
   * accepted or rejected event; for a cancel: cancelled or cancel-rejected).
   * A clientRef is assigned if the command has none. Rejects on timeout, or with
   * code 'ECONNLOST' if the connection drops first (the outcome is then unknown).
   */
  request(command, { timeoutMs = 5000 } = {}) {
    return new Promise((resolve, reject) => {
      const clientRef = command.clientRef ?? this.#nextRef++;
      if (this.#pending.has(clientRef)) {
        reject(new Error(`clientRef ${clientRef} is already in flight`));
        return;
      }
      try {
        this.send({ ...command, clientRef });
      } catch (err) {
        reject(err);
        return;
      }
      const timer = setTimeout(() => {
        this.#pending.delete(clientRef);
        reject(engineError(`no reply from the engine within ${timeoutMs} ms`, 'ETIMEDOUT'));
      }, timeoutMs);
      this.#pending.set(clientRef, { resolve, reject, timer });
    });
  }

  /**
   * Sends a query and resolves with the events of its answer, in order, without the final
   * QUERY_DONE marker. Rejects with code 'QUERY_REJECTED' (and a numeric `reason`) if the
   * engine refuses it, with 'ETIMEDOUT', or with 'ECONNLOST' if the connection drops first.
   * Commands: { kind: 'queryAccount', user } and { kind: 'queryBook', symbol, depth }.
   */
  query(command, { timeoutMs = 5000 } = {}) {
    return new Promise((resolve, reject) => {
      const clientRef = command.clientRef ?? this.#nextRef++;
      if (this.#queries.has(clientRef) || this.#pending.has(clientRef)) {
        reject(new Error(`clientRef ${clientRef} is already in flight`));
        return;
      }
      try {
        this.send({ ...command, clientRef });
      } catch (err) {
        reject(err);
        return;
      }
      const timer = setTimeout(() => {
        this.#queries.delete(clientRef);
        reject(engineError(`no reply from the engine within ${timeoutMs} ms`, 'ETIMEDOUT'));
      }, timeoutMs);
      this.#queries.set(clientRef, { events: [], resolve, reject, timer });
    });
  }

  /** Closes the connection for good. Resolves once the socket is closed. */
  stop() {
    this.#stopped = true;
    if (this.#retryTimer) {
      clearTimeout(this.#retryTimer);
      this.#retryTimer = null;
    }
    this.#rejectPending(engineError('the client was stopped', 'ESTOPPED'));
    this.#failStart(engineError('the client was stopped before it connected', 'ESTOPPED'));
    const socket = this.#socket;
    if (!socket) return Promise.resolve();
    return new Promise((resolve) => {
      socket.once('close', resolve);
      socket.destroy();
    });
  }

  // ---- internals ----

  #connect() {
    if (this.#stopped) return;
    const socket = net.createConnection({ host: this.#host, port: this.#port });
    socket.setNoDelay(true);
    const reader = new FrameReader();
    this.#socket = socket;

    socket.on('connect', () => {
      if (socket !== this.#socket) return;
      this.#up = true;
      this.#delayMs = this.#minDelayMs;
      if (this.#startSettle) {
        this.#startSettle.resolve();
        this.#startSettle = null;
      }
      this.emit('connected');
    });
    socket.on('data', (chunk) => {
      if (socket === this.#socket) this.#onData(socket, reader, chunk);
    });
    socket.on('error', (err) => {
      if (socket === this.#socket) this.emit('problem', err); // 'close' follows
    });
    socket.on('close', () => {
      if (socket === this.#socket) this.#onClose();
    });
  }

  #onData(socket, reader, chunk) {
    const events = [];
    let failure = null;
    try {
      for (const frame of reader.push(chunk)) events.push(decodeEventFrame(frame));
    } catch (err) {
      failure = err;
    }
    // Deliver what decoded cleanly, outside the try block: a bug in a listener must not
    // be mistaken for a protocol error and drop the connection.
    for (const event of events) {
      this.#settle(event);
      this.emit('event', event);
    }
    if (failure) {
      this.emit('problem', failure);
      socket.destroy(); // the stream is broken: 'close' follows and we reconnect
    }
  }

  #settle(event) {
    if (QUERY_ANSWER.has(event.type)) {
      this.#settleQuery(event);
      return;
    }
    if (event.type === EventType.TRADE) return; // a trade is never the acknowledgement
    const entry = this.#pending.get(event.clientRef);
    if (!entry) return;
    this.#pending.delete(event.clientRef);
    clearTimeout(entry.timer);
    entry.resolve(event);
  }

  #settleQuery(event) {
    const entry = this.#queries.get(event.clientRef);
    if (!entry) return; // not ours, or it already timed out
    if (event.type === EventType.QUERY_REJECTED) {
      this.#queries.delete(event.clientRef);
      clearTimeout(entry.timer);
      const err = engineError('the engine refused the query', 'QUERY_REJECTED');
      err.reason = event.reason;
      entry.reject(err);
    } else if (event.type === EventType.QUERY_DONE) {
      this.#queries.delete(event.clientRef);
      clearTimeout(entry.timer);
      entry.resolve(entry.events);
    } else {
      entry.events.push(event);
    }
  }

  #onClose() {
    const wasUp = this.#up;
    this.#socket = null;
    this.#up = false;
    this.#rejectPending(
      engineError('the connection to the engine was lost; the outcome of the request is unknown', 'ECONNLOST'),
    );
    if (wasUp) this.emit('disconnected');
    if (this.#stopped) return;
    if (!this.#reconnect) {
      this.#failStart(engineError('could not connect to the engine', 'ECONNREFUSED'));
      return;
    }
    this.#retryTimer = setTimeout(() => {
      this.#retryTimer = null;
      this.#connect();
    }, this.#delayMs);
    this.#delayMs = Math.min(this.#delayMs * 2, this.#maxDelayMs);
  }

  #rejectPending(err) {
    for (const entry of this.#pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
    this.#pending.clear();
    for (const entry of this.#queries.values()) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
    this.#queries.clear();
  }

  #failStart(err) {
    if (!this.#startSettle) return;
    this.#startSettle.reject(err);
    this.#startSettle = null;
  }
}