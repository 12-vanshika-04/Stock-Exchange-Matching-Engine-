#include <gtest/gtest.h>
#include <utility>
#include <vector>
#include "BookSide.hpp"

using namespace exch;
using Levels = std::vector<std::pair<Price, Qty>>;

static const Price kLow  = toPaise(400);
static const Price kHigh = toPaise(600);

static Order ord(OrderId id, Side side, Price px, Qty qty) {
    return Order{id, 1, 0, side, OrderType::LIMIT, OrderStatus::NEW, px, qty, qty, id};
}

static Levels walk(const BookSide& s) {
    Levels out;
    s.forEachLevel([&](Price p, Qty q) { out.emplace_back(p, q); return true; });
    return out;
}

TEST(BookSide, StartsEmpty) {
    BookSide bids(Side::BUY, kLow, kHigh);
    EXPECT_TRUE(bids.empty());
    EXPECT_TRUE(walk(bids).empty());
}

TEST(BookSide, BestBidIsHighestPrice) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(498), 100));
    bids.add(ord(2, Side::BUY, toPaise(500), 100));
    bids.add(ord(3, Side::BUY, toPaise(499), 100));
    EXPECT_EQ(bids.bestPrice(), toPaise(500));
}

TEST(BookSide, BestAskIsLowestPrice) {
    BookSide asks(Side::SELL, kLow, kHigh);
    asks.add(ord(1, Side::SELL, toPaise(503), 100));
    asks.add(ord(2, Side::SELL, toPaise(501), 100));
    asks.add(ord(3, Side::SELL, toPaise(502), 100));
    EXPECT_EQ(asks.bestPrice(), toPaise(501));
}

TEST(BookSide, SamePriceKeepsArrivalOrder) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.add(ord(2, Side::BUY, toPaise(500), 200));
    EXPECT_EQ(bids.front().id, 1u);
    bids.fillFront(100);
    EXPECT_EQ(bids.front().id, 2u);
}

TEST(BookSide, EmptyingBestLevelPromotesNextBest) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.add(ord(2, Side::BUY, toPaise(499), 100));
    bids.fillFront(100);
    EXPECT_EQ(bids.bestPrice(), toPaise(499));
    bids.fillFront(100);
    EXPECT_TRUE(bids.empty());
}

TEST(BookSide, PromotionSkipsEmptyGaps) {
    BookSide asks(Side::SELL, kLow, kHigh);
    asks.add(ord(1, Side::SELL, toPaise(501), 100));
    asks.add(ord(2, Side::SELL, toPaise(520), 100));
    asks.fillFront(100);
    EXPECT_EQ(asks.bestPrice(), toPaise(520));      // 19 empty rupees in between
}

TEST(BookSide, RemoveBestLevelPromotesNextBest) {
    BookSide bids(Side::BUY, kLow, kHigh);
    auto it = bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.add(ord(2, Side::BUY, toPaise(499), 100));
    bids.remove(toPaise(500), it);
    EXPECT_EQ(bids.bestPrice(), toPaise(499));
}

TEST(BookSide, RemoveNonBestLeavesBestAlone) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(500), 100));
    auto it = bids.add(ord(2, Side::BUY, toPaise(499), 100));
    bids.remove(toPaise(499), it);
    EXPECT_EQ(bids.bestPrice(), toPaise(500));
    Levels expected{{toPaise(500), 100}};
    EXPECT_EQ(walk(bids), expected);
}

TEST(BookSide, PartialFillKeepsLevelAndBest) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.fillFront(40);
    EXPECT_EQ(bids.bestPrice(), toPaise(500));
    EXPECT_EQ(bids.front().remaining, 60);
    Levels expected{{toPaise(500), 60}};
    EXPECT_EQ(walk(bids), expected);
}

TEST(BookSide, WalksLevelsBestFirstSkippingGaps) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(490), 50));
    bids.add(ord(2, Side::BUY, toPaise(500), 100));
    bids.add(ord(3, Side::BUY, toPaise(500), 200));
    Levels expected{{toPaise(500), 300}, {toPaise(490), 50}};
    EXPECT_EQ(walk(bids), expected);
}

TEST(BookSide, AsksWalkLowestFirst) {
    BookSide asks(Side::SELL, kLow, kHigh);
    asks.add(ord(1, Side::SELL, toPaise(503), 10));
    asks.add(ord(2, Side::SELL, toPaise(501), 20));
    Levels expected{{toPaise(501), 20}, {toPaise(503), 10}};
    EXPECT_EQ(walk(asks), expected);
}

TEST(BookSide, WalkStopsWhenVisitorSaysStop) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.add(ord(2, Side::BUY, toPaise(499), 100));
    int visited = 0;
    bids.forEachLevel([&](Price, Qty) { ++visited; return false; });
    EXPECT_EQ(visited, 1);
}

TEST(BookSide, BandEdgesAreUsable) {
    BookSide bids(Side::BUY, kLow, kHigh);
    bids.add(ord(1, Side::BUY, kLow, 10));
    bids.add(ord(2, Side::BUY, kHigh, 10));
    EXPECT_EQ(bids.bestPrice(), kHigh);
    bids.fillFront(10);
    EXPECT_EQ(bids.bestPrice(), kLow);
}
TEST(BookSide, UsableAgainAfterGoingEmpty) {
    BookSide bids(Side::BUY, kLow, kHigh);
    auto it = bids.add(ord(1, Side::BUY, toPaise(500), 100));
    bids.remove(toPaise(500), it);
    EXPECT_TRUE(bids.empty());
    bids.add(ord(2, Side::BUY, toPaise(450), 100));
    EXPECT_EQ(bids.bestPrice(), toPaise(450));
    bids.add(ord(3, Side::BUY, toPaise(455), 100));
    EXPECT_EQ(bids.bestPrice(), toPaise(455));
}