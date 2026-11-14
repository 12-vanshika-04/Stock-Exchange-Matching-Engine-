#pragma once
#include <algorithm>
#include <cassert>
#include <list>
#include "Types.hpp"

namespace exch {

class PriceLevel {
public:
    using Iterator = std::list<Order>::iterator;

    explicit PriceLevel(Price price) : price_(price) {}

    Price       price()      const { return price_; }
    Qty         totalQty()   const { return totalQty_; }
    bool        empty()      const { return orders_.empty(); }
    std::size_t orderCount() const { return orders_.size(); }

    // New orders always go to the back: later arrival = lower priority.
    Iterator add(const Order& o) {
        totalQty_ += o.remaining;
        orders_.push_back(o);
        return std::prev(orders_.end());
    }

    // The order with the highest time priority.
    Order& front() {
        assert(!orders_.empty());
        return orders_.front();
    }

    // Fill `qty` shares of the front order. Removes it if fully filled.
    void fillFront(Qty qty) {
        Order& o = front();
        assert(qty > 0 && qty <= o.remaining);
        o.remaining -= qty;
        totalQty_   -= qty;
        if (o.remaining == 0) {
            o.status = OrderStatus::FILLED;
            orders_.pop_front();
        } else {
            o.status = OrderStatus::PARTIAL;
        }
    }

    // Cancel an order anywhere in the queue, using the handle from add().
    void remove(Iterator it) {
        totalQty_ -= it->remaining;
        orders_.erase(it);
    }

private:
    Price            price_;
    Qty              totalQty_ = 0;
    std::list<Order> orders_;
};

}  // namespace exch