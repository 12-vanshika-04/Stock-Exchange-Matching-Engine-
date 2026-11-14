#pragma once
#include <cstddef>
#include "Exchange.hpp"
#include "Messages.hpp"
#include "Types.hpp"

namespace exch {

// Runs one command at a time to completion. No threads, no I/O: the engine thread
// (next step) just feeds it commands and forwards its events.
class EngineCore {
public:
        explicit EngineCore(std::size_t numSymbols, const std::vector<Price>& referencePrices = {})
        : exchange_(numSymbols, referencePrices) {}

    Exchange&       exchange()       { return exchange_; }
    const Exchange& exchange() const { return exchange_; }

    // Every resulting event is passed to emit(const Event&), in order.
     template <typename Sink>
    void process(const Command& c, Sink&& emit) {
        switch (c.type) {
            case CommandType::NEW_ORDER:     newOrder(c, emit);     break;
            case CommandType::CANCEL_ORDER:  cancelOrder(c, emit);  break;
            case CommandType::QUERY_ACCOUNT: queryAccount(c, emit); break;
            case CommandType::QUERY_BOOK:    queryBook(c, emit);    break;
        }
    }

private:
    template <typename Sink>
    void newOrder(const Command& c, Sink& emit) {
        const ExchangeResult r =
            exchange_.submit(c.user, c.symbol, c.side, c.orderType, c.price, c.qty);

        Event e;
        e.symbol    = c.symbol;
        e.user      = c.user;
        e.side      = c.side;
        e.clientRef = c.clientRef;
        e.orderId   = r.order.id;
        e.price     = r.order.price;
        e.qty       = r.order.qty;
        e.remaining = r.order.remaining;

        if (r.reason != RejectReason::NONE) {
            e.type   = EventType::ORDER_REJECTED;
            e.reason = r.reason;
            e.status = OrderStatus::REJECTED;
            emit(e);
            return;
        }

        e.type   = EventType::ORDER_ACCEPTED;
        e.status = r.order.status;
        emit(e);

        for (const Trade& t : r.trades) {
            Event te;
            te.type      = EventType::TRADE;
            te.symbol    = t.symbolId;
            te.price     = t.price;
            te.qty       = t.qty;
            te.clientRef = c.clientRef;
            te.trade     = t;
            emit(te);
        }
    }

    template <typename Sink>
    void cancelOrder(const Command& c, Sink& emit) {
        Event e;
        e.symbol    = c.symbol;
        e.user      = c.user;
        e.orderId   = c.orderId;
        e.clientRef = c.clientRef;
        if (exchange_.cancel(c.user, c.symbol, c.orderId)) {
            e.type   = EventType::ORDER_CANCELLED;
            e.status = OrderStatus::CANCELLED;
        } else {
            e.type = EventType::CANCEL_REJECTED;   // unknown order, or not yours
        }
        emit(e);
    }
        template <typename Sink>
    void rejectQuery(const Command& c, RejectReason why, Sink& emit) {
        Event e;
        e.type      = EventType::QUERY_REJECTED;
        e.reason    = why;
        e.symbol    = c.symbol;
        e.user      = c.user;
        e.clientRef = c.clientRef;
        emit(e);
    }

    template <typename Sink>
    void queryAccount(const Command& c, Sink& emit) {
        const AccountManager& accounts = exchange_.accounts();
        if (!accounts.exists(c.user)) {
            rejectQuery(c, RejectReason::UNKNOWN_USER, emit);
            return;
        }
        const Account& a = accounts.account(c.user);

        Event balance;
        balance.type      = EventType::ACCOUNT_BALANCE;
        balance.user      = c.user;
        balance.price     = a.cash;
        balance.qty       = a.reservedCash;
        balance.clientRef = c.clientRef;
        emit(balance);

        for (std::size_t s = 0; s < a.positions.size(); ++s) {
            const Position& p = a.positions[s];
            Event pos;
            pos.type      = EventType::ACCOUNT_POSITION;
            pos.user      = c.user;
            pos.symbol    = static_cast<SymbolId>(s);
            pos.qty       = p.qty;
            pos.remaining = p.reservedQty;
            pos.price     = p.costBasis;
            pos.clientRef = c.clientRef;
            emit(pos);
        }

        Event done;
        done.type      = EventType::QUERY_DONE;
        done.user      = c.user;
        done.clientRef = c.clientRef;
        emit(done);
    }

    template <typename Sink>
    void queryBook(const Command& c, Sink& emit) {
        if (c.symbol >= exchange_.numSymbols()) {
            rejectQuery(c, RejectReason::UNKNOWN_SYMBOL, emit);
            return;
        }
        const OrderBook& book = exchange_.engine().book(c.symbol);
        const std::size_t depth = static_cast<std::size_t>(
            c.qty < 1 ? 1 : (c.qty > kMaxBookDepth ? kMaxBookDepth : c.qty));

        for (const Side side : {Side::BUY, Side::SELL}) {
            std::size_t sent = 0;
            book.forEachLevel(side, [&](Price price, Qty qty) {
                Event level;
                level.type      = EventType::BOOK_LEVEL;
                level.symbol    = c.symbol;
                level.side      = side;
                level.price     = price;
                level.qty       = qty;
                level.clientRef = c.clientRef;
                emit(level);
                return ++sent < depth;                 // keep walking until `depth` levels went out
            });
        }

        Event done;
        done.type      = EventType::QUERY_DONE;
        done.symbol    = c.symbol;
        done.clientRef = c.clientRef;
        emit(done);
    }

    Exchange exchange_;
};

}  // namespace exch