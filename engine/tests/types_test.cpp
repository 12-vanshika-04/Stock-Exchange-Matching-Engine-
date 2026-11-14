#include <gtest/gtest.h>
#include "Types.hpp"

using namespace exch;

TEST(Money, FloatingPointIsNotExact) {
    // This is why we never store prices as double.
    EXPECT_NE(0.1 + 0.2, 0.3);
}

TEST(Money, PaiseIsExact) {
    EXPECT_EQ(toPaise(500), 50000);
    EXPECT_EQ(toPaise(499, 50), 49950);
    EXPECT_EQ(toPaise(0, 5), 5);                    // Rs 0.05 tick
    EXPECT_EQ(toPaise(0, 10) + toPaise(0, 20), toPaise(0, 30));
}

TEST(OrderType, NewOrderStartsFullyUnfilled) {
    Order o{1, 7, 0, Side::BUY, OrderType::LIMIT, OrderStatus::NEW,
            toPaise(500), 100, 100, 1};
    EXPECT_EQ(o.remaining, o.qty);
    EXPECT_EQ(o.status, OrderStatus::NEW);
}

TEST(OrderType, PartialFillReducesRemainingOnly) {
    Order o{2, 7, 0, Side::SELL, OrderType::LIMIT, OrderStatus::NEW,
            toPaise(500), 250, 250, 2};
    o.remaining -= 100;
    o.status = OrderStatus::PARTIAL;
    EXPECT_EQ(o.qty, 250);
    EXPECT_EQ(o.remaining, 150);
}