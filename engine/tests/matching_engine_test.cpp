#include <gtest/gtest.h>
#include "MatchingEngine.hpp"

using namespace exch;
using Depth = std::vector<std::pair<Price, Qty>>;

static Order lim(OrderId id, UserId user, Side side, Price px, Qty qty) {
    return Order{id, user, 0, side, OrderType::LIMIT, OrderStatus::NEW,
                 px, qty, qty, 0};
}
static Order mkt(OrderId id, UserId user, Side side, Qty qty) {
    return Order{id, user, 0, side, OrderType::MARKET, OrderStatus::NEW,
                 0, qty, qty, 0};
}

TEST(Matching, LimitRestsWhenBookIsEmpty) {
    MatchingEngine e(1);
    auto r = e.submit(lim(1, 10, Side::BUY, toPaise(500), 100));
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(r.order.status, OrderStatus::NEW);
    EXPECT_EQ(e.book(0).bestBidPrice(), toPaise(500));
}

TEST(Matching, NonCrossingLimitRests) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto r = e.submit(lim(2, 20, Side::BUY, toPaise(500), 100));
    EXPECT_TRUE(r.trades.empty());
    EXPECT_TRUE(e.book(0).hasBids());
    EXPECT_TRUE(e.book(0).hasAsks());
}

TEST(Matching, ExactFillClearsBothSides) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto r = e.submit(lim(2, 20, Side::BUY, toPaise(501), 100));

    ASSERT_EQ(r.trades.size(), 1u);
    const Trade& t = r.trades[0];
    EXPECT_EQ(t.price, toPaise(501));
    EXPECT_EQ(t.qty, 100);
    EXPECT_EQ(t.buyOrderId, 2u);
    EXPECT_EQ(t.sellOrderId, 1u);
    EXPECT_EQ(t.buyerId, 20u);
    EXPECT_EQ(t.sellerId, 10u);
    EXPECT_EQ(r.order.status, OrderStatus::FILLED);
    EXPECT_EQ(e.book(0).orderCount(), 0u);
}

TEST(Matching, TradeHappensAtRestingPrice) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto r = e.submit(lim(2, 20, Side::BUY, toPaise(505), 100));  // willing to pay 505
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, toPaise(501));                   // but pays 501
}

TEST(Matching, PartialFillRemainderRestsInBook) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto r = e.submit(lim(2, 20, Side::BUY, toPaise(501), 150));

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].qty, 100);
    EXPECT_EQ(r.order.status, OrderStatus::PARTIAL);
    EXPECT_EQ(r.order.remaining, 50);
    EXPECT_FALSE(e.book(0).hasAsks());
    EXPECT_EQ(e.book(0).bestBidPrice(), toPaise(501));
    Depth bids{{toPaise(501), 50}};
    EXPECT_EQ(e.book(0).depth(Side::BUY, 10), bids);
}

TEST(Matching, PartialFillLeavesRestingRemainder) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 200));
    auto r = e.submit(lim(2, 20, Side::BUY, toPaise(501), 50));

    EXPECT_EQ(r.order.status, OrderStatus::FILLED);
    EXPECT_TRUE(e.book(0).contains(1));
    Depth asks{{toPaise(501), 150}};
    EXPECT_EQ(e.book(0).depth(Side::SELL, 10), asks);
}

TEST(Matching, BuySweepsMultipleAskLevelsLowestFirst) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    e.submit(lim(2, 11, Side::SELL, toPaise(502), 250));
    auto r = e.submit(lim(3, 20, Side::BUY, toPaise(502), 300));

    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].price, toPaise(501));
    EXPECT_EQ(r.trades[0].qty, 100);
    EXPECT_EQ(r.trades[1].price, toPaise(502));
    EXPECT_EQ(r.trades[1].qty, 200);
    Depth asks{{toPaise(502), 50}};
    EXPECT_EQ(e.book(0).depth(Side::SELL, 10), asks);
}

TEST(Matching, SellSweepsBidLevelsHighestFirst) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::BUY, toPaise(500), 100));
    e.submit(lim(2, 11, Side::BUY, toPaise(499), 200));
    auto r = e.submit(lim(3, 20, Side::SELL, toPaise(499), 250));

    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].price, toPaise(500));
    EXPECT_EQ(r.trades[0].buyOrderId, 1u);
    EXPECT_EQ(r.trades[1].price, toPaise(499));
    EXPECT_EQ(r.trades[1].qty, 150);
    EXPECT_EQ(r.order.status, OrderStatus::FILLED);
    Depth bids{{toPaise(499), 50}};
    EXPECT_EQ(e.book(0).depth(Side::BUY, 10), bids);
}

// Synopsis section 7: orders A, B, C all bid Rs 500; SELL 250 @ 500 arrives.
TEST(Matching, SynopsisPriceTimePriority) {
    MatchingEngine e(1);
    e.submit(lim(1, 1, Side::BUY, toPaise(500), 100));   // A
    e.submit(lim(2, 2, Side::BUY, toPaise(500), 200));   // B
    e.submit(lim(3, 3, Side::BUY, toPaise(500), 150));   // C
    auto r = e.submit(lim(9, 9, Side::SELL, toPaise(500), 250));

    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].buyOrderId, 1u);   // A first
    EXPECT_EQ(r.trades[0].qty, 100);
    EXPECT_EQ(r.trades[1].buyOrderId, 2u);   // then B
    EXPECT_EQ(r.trades[1].qty, 150);

    EXPECT_FALSE(e.book(0).contains(1));     // A fully filled
    EXPECT_TRUE(e.book(0).contains(2));      // B keeps 50
    EXPECT_TRUE(e.book(0).contains(3));      // C untouched
    Depth bids{{toPaise(500), 200}};         // B's 50 + C's 150
    EXPECT_EQ(e.book(0).depth(Side::BUY, 10), bids);
}

// Synopsis section 6: the starting book, then SELL 150 @ 500.
TEST(Matching, SynopsisOrderBookExample) {
    MatchingEngine e(1);
    e.submit(lim(1, 1, Side::BUY,  toPaise(500), 200));
    e.submit(lim(2, 2, Side::BUY,  toPaise(499), 300));
    e.submit(lim(3, 3, Side::BUY,  toPaise(498), 500));
    e.submit(lim(4, 4, Side::SELL, toPaise(501), 100));
    e.submit(lim(5, 5, Side::SELL, toPaise(502), 250));
    e.submit(lim(6, 6, Side::SELL, toPaise(503), 400));

    auto r = e.submit(lim(9, 9, Side::SELL, toPaise(500), 150));

    // The bid has 200 shares, so all 150 execute (the synopsis text says 100).
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, toPaise(500));
    EXPECT_EQ(r.trades[0].qty, 150);
    EXPECT_EQ(r.order.status, OrderStatus::FILLED);

    Depth bids{{toPaise(500), 50}, {toPaise(499), 300}, {toPaise(498), 500}};
    EXPECT_EQ(e.book(0).depth(Side::BUY, 10), bids);
}

TEST(Matching, MarketBuyWalksTheBook) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    e.submit(lim(2, 11, Side::SELL, toPaise(502), 250));
    auto r = e.submit(mkt(3, 20, Side::BUY, 150));

    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].price, toPaise(501));
    EXPECT_EQ(r.trades[0].qty, 100);
    EXPECT_EQ(r.trades[1].price, toPaise(502));
    EXPECT_EQ(r.trades[1].qty, 50);
    EXPECT_EQ(r.order.status, OrderStatus::FILLED);
    Depth asks{{toPaise(502), 200}};
    EXPECT_EQ(e.book(0).depth(Side::SELL, 10), asks);
}

TEST(Matching, MarketRemainderIsCancelledNotRested) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto r = e.submit(mkt(2, 20, Side::BUY, 300));

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].qty, 100);
    EXPECT_EQ(r.order.status, OrderStatus::CANCELLED);
    EXPECT_EQ(r.order.remaining, 200);
    EXPECT_FALSE(e.book(0).hasBids());       // nothing rested
    EXPECT_EQ(e.book(0).orderCount(), 0u);
}

TEST(Matching, MarketOrderOnEmptyBookIsCancelled) {
    MatchingEngine e(1);
    auto r = e.submit(mkt(1, 10, Side::SELL, 100));
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(r.order.status, OrderStatus::CANCELLED);
    EXPECT_EQ(e.book(0).orderCount(), 0u);
}

TEST(Matching, CancelRestingOrder) {
    MatchingEngine e(1);
    e.submit(lim(1, 10, Side::BUY, toPaise(500), 100));
    EXPECT_TRUE(e.cancel(0, 1));
    EXPECT_FALSE(e.book(0).hasBids());
    EXPECT_FALSE(e.cancel(0, 1));            // already gone
}

TEST(Matching, SequenceAndTradeIdsIncrease) {
    MatchingEngine e(1);
    auto a = e.submit(lim(1, 10, Side::SELL, toPaise(501), 100));
    auto b = e.submit(lim(2, 20, Side::BUY,  toPaise(501), 40));
    auto c = e.submit(lim(3, 20, Side::BUY,  toPaise(501), 40));
    EXPECT_EQ(a.order.seq, 1u);
    EXPECT_EQ(b.order.seq, 2u);
    EXPECT_EQ(c.order.seq, 3u);
    EXPECT_EQ(b.trades[0].tradeId, 1u);
    EXPECT_EQ(c.trades[0].tradeId, 2u);
}