import { useCallback, useEffect, useReducer, useState } from 'react';
import { io } from 'socket.io-client';
import { SYMBOLS } from '../lib/symbols.js';
import { emptyTrades, tradesReducer } from '../lib/trades.js';
import { initialOrders, ordersReducer } from '../lib/orders.js';

/**
 * Connects to the gateway as `userId` (the development login) and keeps:
 *   trades  the public trades for every symbol
 *   orders  this account's orders and fills (private events), see lib/orders.js
 *   gap     true once the connection dropped: updates may be missing from then on
 * status: 'connecting' | 'connected' | 'disconnected' | 'refused'.
 */
export function useGateway(userId) {
  const [status, setStatus] = useState('connecting');
  const [gap, setGap] = useState(false);
  const [trades, dispatchTrades] = useReducer(tradesReducer, SYMBOLS, emptyTrades);
  const [orders, dispatchOrders] = useReducer(ordersReducer, null, initialOrders);

  useEffect(() => {
    dispatchTrades({ type: 'clear' });
    dispatchOrders({ type: 'clear' });
    setGap(false);
    setStatus('connecting');
    const socket = io({ auth: { userId: String(userId) } }); // same origin: Vite proxies it to the gateway

    socket.on('connect', () => {
      setStatus('connected');
      // Runs on every (re)connect: the reply replaces the list, so a reconnect never leaves gaps or repeats.
      socket.emit('subscribe', SYMBOLS, (reply) => {
        if (reply?.ok) dispatchTrades({ type: 'snapshot', recent: reply.recent });
      });
    });
    socket.on('disconnect', (reason) => {
      if (reason === 'io client disconnect') return; // we closed it ourselves (account switch)
      setStatus('disconnected');
      setGap(true);
    });
    socket.on('connect_error', (err) => setStatus(err.message === 'UNAUTHENTICATED' ? 'refused' : 'disconnected'));

    socket.on('trade', (trade) => dispatchTrades({ type: 'trade', trade }));
    socket.on('order', (order) => dispatchOrders({ type: 'accepted', order }));
    socket.on('fill', (fill) => dispatchOrders({ type: 'fill', fill }));
    socket.on('order:cancelled', (e) => dispatchOrders({ type: 'cancelled', orderId: e.orderId }));

    return () => {
      socket.disconnect();
    };
  }, [userId]);

  // The REST replies are recorded too, so an order shows up even if the socket missed it.
  const recordOrder = useCallback((order) => dispatchOrders({ type: 'accepted', order }), []);
  const recordCancel = useCallback((orderId) => dispatchOrders({ type: 'cancelled', orderId }), []);

  return { status, trades, orders, gap, recordOrder, recordCancel };
}