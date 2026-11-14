#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>
#include "OrderBook.hpp"
#include "Types.hpp"

namespace exch {
// The lowest and highest limit price accepted for one symbol (paise, on the tick).
struct PriceBand {
    Price low;
    Price high;
};
struct SubmitResult {
    Order              order;    // final state of the incoming order
    std::vector<Trade> trades;   // trades produced, in execution order
};

class MatchingEngine {
public:
   explicit MatchingEngine(std::size_t numSymbols,
                            Price bandLow = toPaise(400), Price bandHigh = toPaise(600)) {
        books_.reserve(numSymbols);
        for (std::size_t i = 0; i < numSymbols; ++i)
            books_.emplace_back(static_cast<SymbolId>(i), bandLow, bandHigh);
    }
        // One band per symbol.
    explicit MatchingEngine(const std::vector<PriceBand>& bands) {
        books_.reserve(bands.size());
        for (std::size_t i = 0; i < bands.size(); ++i)
            books_.emplace_back(static_cast<SymbolId>(i), bands[i].low, bands[i].high);
    }
    const OrderBook& book(SymbolId s) const { return books_[s]; }

    SubmitResult submit(Order o) {
        assert(o.symbolId < books_.size());
        assert(o.qty > 0);

        o.seq       = ++nextSeq_;      // arrival order = time priority
        o.remaining = o.qty;
        o.status    = OrderStatus::NEW;

        OrderBook& book = books_[o.symbolId];
        SubmitResult res{o, {}};
        Order& in = res.order;

        const Side opp = (in.side == Side::BUY) ? Side::SELL : Side::BUY;

        while (in.remaining > 0 && hasLiquidity(book, opp)) {
            Order& resting = book.front(opp);
            if (in.type == OrderType::LIMIT && !crosses(in, resting.price))
                break;

            // Copy what we need BEFORE filling: fillFront may remove `resting`.
            const Price   px          = resting.price;
            const OrderId restingId   = resting.id;
            const UserId  restingUser = resting.userId;
            const Qty     q           = std::min(in.remaining, resting.remaining);

            const bool inIsBuy = (in.side == Side::BUY);
            res.trades.push_back(Trade{
                ++nextTradeId_, in.symbolId, px, q,
                inIsBuy ? in.id : restingId,        // buyOrderId
                inIsBuy ? restingId : in.id,        // sellOrderId
                inIsBuy ? in.userId : restingUser,  // buyerId
                inIsBuy ? restingUser : in.userId,  // sellerId
                in.seq});

            book.fillFront(opp, q);
            in.remaining -= q;
        }

        if (in.remaining == 0) {
            in.status = OrderStatus::FILLED;
        } else if (in.type == OrderType::LIMIT) {
            in.status = (in.remaining < in.qty) ? OrderStatus::PARTIAL
                                                : OrderStatus::NEW;
            book.add(in);                           // leftover rests in the book
        } else {
            in.status = OrderStatus::CANCELLED;     // market order: unfilled part dropped
        }
        return res;
    }

    bool cancel(SymbolId symbol, OrderId id) {
        return books_[symbol].cancel(id);
    }

private:
    static bool hasLiquidity(const OrderBook& b, Side oppositeSide) {
        return oppositeSide == Side::SELL ? b.hasAsks() : b.hasBids();
    }

    // Does the incoming limit order's price reach the resting price?
    static bool crosses(const Order& in, Price restingPrice) {
        return in.side == Side::BUY ? in.price >= restingPrice
                                    : in.price <= restingPrice;
    }

    std::vector<OrderBook> books_;
    SeqNo         nextSeq_     = 0;
    std::uint64_t nextTradeId_ = 0;
};

}  // namespace exch