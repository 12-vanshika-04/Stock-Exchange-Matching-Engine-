#include <gtest/gtest.h>
#include "PriceLevel.hpp"

using namespace exch;

static Order makeOrder(OrderId id, Qty qty, SeqNo seq) {
    return Order{id, 1, 0, Side::BUY, OrderType::LIMIT, OrderStatus::NEW,
                 toPaise(500), qty, qty, seq};
}

TEST(PriceLevel, StartsEmpty) {
    PriceLevel level(toPaise(500));
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.totalQty(), 0);
}

TEST(PriceLevel, TracksTotalQuantity) {
    PriceLevel level(toPaise(500));
    level.add(makeOrder(1, 100, 1));
    level.add(makeOrder(2, 200, 2));
    EXPECT_EQ(level.totalQty(), 300);
    EXPECT_EQ(level.orderCount(), 2u);
}

TEST(PriceLevel, FirstInIsFirstOut) {
    PriceLevel level(toPaise(500));
    level.add(makeOrder(10, 100, 1));
    level.add(makeOrder(20, 100, 2));
    level.add(makeOrder(30, 100, 3));

    EXPECT_EQ(level.front().id, 10u);
    level.fillFront(100);
    EXPECT_EQ(level.front().id, 20u);
    level.fillFront(100);
    EXPECT_EQ(level.front().id, 30u);
}

TEST(PriceLevel, PartialFillKeepsPlaceInQueue) {
    PriceLevel level(toPaise(500));
    level.add(makeOrder(1, 100, 1));
    level.add(makeOrder(2, 100, 2));

    level.fillFront(40);                       // order 1 partly filled
    EXPECT_EQ(level.front().id, 1u);           // still first in line
    EXPECT_EQ(level.front().remaining, 60);
    EXPECT_EQ(level.front().status, OrderStatus::PARTIAL);
    EXPECT_EQ(level.totalQty(), 160);
}

TEST(PriceLevel, CancelFromMiddle) {
    PriceLevel level(toPaise(500));
    level.add(makeOrder(1, 100, 1));
    auto mid = level.add(makeOrder(2, 200, 2));
    level.add(makeOrder(3, 150, 3));

    level.remove(mid);
    EXPECT_EQ(level.totalQty(), 250);
    EXPECT_EQ(level.orderCount(), 2u);
    level.fillFront(100);
    EXPECT_EQ(level.front().id, 3u);           // order 2 is gone
}

// The exact example from your synopsis (section 7).
TEST(PriceLevel, SynopsisPriceTimeExample) {
    PriceLevel level(toPaise(500));
    level.add(makeOrder(/*A*/ 1, 100, 1));
    level.add(makeOrder(/*B*/ 2, 200, 2));
    level.add(makeOrder(/*C*/ 3, 150, 3));

    // Incoming SELL of 250 @ Rs 500 consumes the level in priority order.
    Qty incoming = 250;
    while (incoming > 0 && !level.empty()) {
        Qty q = std::min(incoming, level.front().remaining);
        level.fillFront(q);
        incoming -= q;
    }

    EXPECT_EQ(incoming, 0);                    // sell fully executed
    EXPECT_EQ(level.front().id, 2u);           // B is next in line
    EXPECT_EQ(level.front().remaining, 50);    // B has 50 left
    EXPECT_EQ(level.totalQty(), 200);          // B's 50 + C's 150
}