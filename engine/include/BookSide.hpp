#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "PriceLevel.hpp"
#include "Types.hpp"

namespace exch {

// One side (all bids, or all asks) of one symbol's book. Price levels live in a
// flat array indexed by (price - low) / tick, so finding a level is arithmetic.
// Callers must pass on-tick prices inside [low, high]; Exchange guarantees that.
class BookSide {
public:
    BookSide(Side side, Price low, Price high)
        : isBid_(side == Side::BUY),
          low_(low),
          count_(static_cast<std::int32_t>((high - low) / kTickSize) + 1) {
        assert(low > 0 && high >= low);
        assert(low % kTickSize == 0 && high % kTickSize == 0);
        levels_.reserve(static_cast<std::size_t>(count_));
        for (std::int32_t i = 0; i < count_; ++i)
            levels_.emplace_back(priceAt(i));
    }

    bool  empty()     const { return best_ == kNone; }
    Price bestPrice() const { assert(!empty()); return priceAt(best_); }

    // The highest-priority resting order on this side.
    Order& front() { assert(!empty()); return at(best_).front(); }

    // Rest an order at its price. Returns a handle for O(1) cancel.
       PriceLevel::Iterator add(const Order& o) {
        const std::int32_t i = indexOf(o.price);
        PriceLevel& lv = at(i);
        const bool wasEmpty = lv.empty();
        const PriceLevel::Iterator it = lv.add(o);
        if (wasEmpty) ++nonEmpty_;
        if (best_ == kNone || isBetter(i, best_)) best_ = i;
        return it;
    }

    // Fill `qty` shares of the best order. Moves `best` along if its level empties.
        void fillFront(Qty qty) {
        assert(!empty());
        PriceLevel& lv = at(best_);
        lv.fillFront(qty);
        if (lv.empty()) {
            --nonEmpty_;
            refreshBest();
        }
    }

    // Remove one order anywhere in the side, using the handle from add().
        void remove(Price price, PriceLevel::Iterator it) {
        const std::int32_t i = indexOf(price);
        PriceLevel& lv = at(i);
        lv.remove(it);
        if (lv.empty()) {
            --nonEmpty_;
            if (i == best_) refreshBest();
        }
    }

    // Visit non-empty levels best-first as (price, totalQty).
    // Stops as soon as `visit` returns false.
    template <typename Visitor>
    void forEachLevel(Visitor&& visit) const {
        if (empty()) return;
        for (std::int32_t i = best_; i >= 0 && i < count_; i += step()) {
            const PriceLevel& lv = at(i);
            if (lv.empty()) continue;
            if (!visit(lv.price(), lv.totalQty())) return;
        }
    }

private:
    static constexpr std::int32_t kNone = -1;

    std::int32_t step() const { return isBid_ ? -1 : 1; }   // direction away from the spread
    bool isBetter(std::int32_t i, std::int32_t j) const { return isBid_ ? i > j : i < j; }
    Price priceAt(std::int32_t i) const { return low_ + static_cast<Price>(i) * kTickSize; }

    std::int32_t indexOf(Price p) const {
        assert(p >= low_ && p % kTickSize == 0);
        const std::int32_t i = static_cast<std::int32_t>((p - low_) / kTickSize);
        assert(i >= 0 && i < count_);
        return i;
    }

    PriceLevel&       at(std::int32_t i)       { return levels_[static_cast<std::size_t>(i)]; }
    const PriceLevel& at(std::int32_t i) const { return levels_[static_cast<std::size_t>(i)]; }

    // The best level just emptied: walk away from the spread to the next non-empty one.
        void refreshBest() {
        if (nonEmpty_ == 0) { best_ = kNone; return; }   // nothing left: no scan needed
        std::int32_t i = best_;
        while (i >= 0 && i < count_ && at(i).empty()) i += step();
        best_ = (i >= 0 && i < count_) ? i : kNone;
    }

    bool                  isBid_;
    Price                 low_;
    std::int32_t          count_;
    std::int32_t          best_ = kNone;
    std::int32_t          nonEmpty_ = 0;   // how many levels currently hold orders
    std::vector<PriceLevel> levels_;
};

}  // namespace exch