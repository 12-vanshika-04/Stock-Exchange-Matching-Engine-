#pragma once
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include "PriceLevel.hpp"
#include "BookSide.hpp"
#include "Types.hpp"
#include <algorithm>


namespace exch {

class OrderBook {
    // Where a resting order lives, so cancel is O(1) instead of a search.
        struct Locator {
        Side                 side;
        bool                 live = false;   // is this id currently resting in the book?
        Price                price;
        PriceLevel::Iterator it;
    };
public:
       explicit OrderBook(SymbolId symbol, Price low = toPaise(400), Price high = toPaise(600))
        : symbol_(symbol), bids_(Side::BUY, low, high), asks_(Side::SELL, low, high) {}

    SymbolId    symbol()     const { return symbol_; }
    bool        hasBids()    const { return !bids_.empty(); }
    bool        hasAsks()    const { return !asks_.empty(); }
        std::size_t orderCount() const { return live_; }
    bool        contains(OrderId id) const { return slot(id) != nullptr; }
    // Read-only lookup of a resting order (nullptr if not in the book).
        const Order* find(OrderId id) const {
        const Locator* loc = slot(id);
        return loc == nullptr ? nullptr : &*loc->it;
    }

       Price bestBidPrice() const { assert(hasBids()); return bids_.bestPrice(); }
    Price bestAskPrice() const { assert(hasAsks()); return asks_.bestPrice(); }

    // Rest a LIMIT order in the book.
    void add(const Order& o) {
        assert(o.type == OrderType::LIMIT);
        assert(o.symbolId == symbol_);
        assert(o.remaining > 0);
        assert(!contains(o.id));

        const PriceLevel::Iterator it = (o.side == Side::BUY) ? bids_.add(o) : asks_.add(o);
            indexInsert(o.id, Locator{o.side, true, o.price, it});
    }

    // Remove a resting order. Returns false if the id is not in the book.
        bool cancel(OrderId id) {
        Locator* loc = slot(id);
        if (loc == nullptr) return false;
        const Locator saved = *loc;
        indexErase(id);
        if (saved.side == Side::BUY) bids_.remove(saved.price, saved.it);
        else                         asks_.remove(saved.price, saved.it);
        return true;
    }

    // Highest-priority resting order on a side (BUY = best bid, SELL = best ask).
        Order& front(Side side) {
        return (side == Side::BUY) ? bids_.front() : asks_.front();
    }

    // Fill `qty` shares of the best resting order on `side`.
    void fillFront(Side side, Qty qty) {
        BookSide& s = (side == Side::BUY) ? bids_ : asks_;
        const Order& o = s.front();
        const OrderId id = o.id;                 // read both BEFORE filling:
        const bool fullyFilled = (qty == o.remaining);   // the order may be destroyed
        s.fillFront(qty);
        if (fullyFilled) indexErase(id);
    }

    // Aggregated depth: (price, total quantity), best price first.
    std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t maxLevels) const {
        std::vector<std::pair<Price, Qty>> out;
        sideOf(side).forEachLevel([&](Price p, Qty q) {
            if (out.size() == maxLevels) return false;
            out.emplace_back(p, q);
            return out.size() < maxLevels;
        });
        return out;
    }

    template <typename Visitor>
    void forEachLevel(Side side, Visitor&& visit) const {
        sideOf(side).forEachLevel(std::forward<Visitor>(visit));
    }

private:
        // Flat index: local order id -> slot. Each symbol numbers its own orders 1, 2, 3...
    Locator* slot(OrderId id) {
        if (symbolOfOrderId(id) != symbol_) return nullptr;   // an id from another symbol
        const std::uint64_t k = localOrderId(id);
        return (k < index_.size() && index_[k].live) ? &index_[k] : nullptr;
    }
    const Locator* slot(OrderId id) const {
        if (symbolOfOrderId(id) != symbol_) return nullptr;
        const std::uint64_t k = localOrderId(id);
        return (k < index_.size() && index_[k].live) ? &index_[k] : nullptr;
    }
    void indexInsert(OrderId id, const Locator& loc) {
        const std::uint64_t k = localOrderId(id);
        if (k >= index_.size())
            index_.resize(std::max<std::size_t>(k + 1, index_.size() * 2));
        index_[k] = loc;
        ++live_;
    }
    void indexErase(OrderId id) {
        index_[localOrderId(id)].live = false;
        --live_;
    }
    const BookSide& sideOf(Side s) const { return s == Side::BUY ? bids_ : asks_; }

    SymbolId symbol_;
    BookSide bids_;
    BookSide asks_;
    std::vector<Locator> index_;       // indexed by order id
    std::size_t          live_ = 0;    // number of resting orders
};

}  // namespace exch