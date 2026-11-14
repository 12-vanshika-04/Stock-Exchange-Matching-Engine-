#include <gtest/gtest.h>
#include "OrderBook.hpp"

using namespace exch;
using Depth = std::vector<std::pair<Price, Qty>>;

static Order lim(OrderId id, Side side, Price px, Qty qty, SeqNo seq) {
    return Order{id, 1, 0, side, OrderType::LIMIT, OrderStatus::NEW,
                 px, qty, qty, seq};
}

TEST(OrderBook, StartsEmpty) {
    OrderBook book(0);
    EXPECT_FALSE(book.hasBids());
    EXPECT_FALSE(book.hasAsks());
    EXPECT_EQ(book.orderCount(), 0u);
}

TEST(OrderBook, BestBidIsHighestPrice) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(498), 100, 1));
    book.add(lim(2, Side::BUY, toPaise(500), 100, 2));
    book.add(lim(3, Side::BUY, toPaise(499), 100, 3));
    EXPECT_EQ(book.bestBidPrice(), toPaise(500));
}

TEST(OrderBook, BestAskIsLowestPrice) {
    OrderBook book(0);
    book.add(lim(1, Side::SELL, toPaise(503), 100, 1));
    book.add(lim(2, Side::SELL, toPaise(501), 100, 2));
    book.add(lim(3, Side::SELL, toPaise(502), 100, 3));
    EXPECT_EQ(book.bestAskPrice(), toPaise(501));
}

TEST(OrderBook, SamePriceOrdersShareOneLevel) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(500), 100, 1));
    book.add(lim(2, Side::BUY, toPaise(500), 200, 2));
    Depth expected{{toPaise(500), 300}};
    EXPECT_EQ(book.depth(Side::BUY, 10), expected);
    EXPECT_EQ(book.front(Side::BUY).id, 1u);        // earlier order first
}

// The starting book from your synopsis (section 6).
TEST(OrderBook, SynopsisInitialBook) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY,  toPaise(500), 200, 1));
    book.add(lim(2, Side::BUY,  toPaise(499), 300, 2));
    book.add(lim(3, Side::BUY,  toPaise(498), 500, 3));
    book.add(lim(4, Side::SELL, toPaise(501), 100, 4));
    book.add(lim(5, Side::SELL, toPaise(502), 250, 5));
    book.add(lim(6, Side::SELL, toPaise(503), 400, 6));

    Depth bids{{toPaise(500), 200}, {toPaise(499), 300}, {toPaise(498), 500}};
    Depth asks{{toPaise(501), 100}, {toPaise(502), 250}, {toPaise(503), 400}};
    EXPECT_EQ(book.depth(Side::BUY, 10), bids);
    EXPECT_EQ(book.depth(Side::SELL, 10), asks);
}

TEST(OrderBook, DepthRespectsLevelLimit) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(500), 100, 1));
    book.add(lim(2, Side::BUY, toPaise(499), 100, 2));
    book.add(lim(3, Side::BUY, toPaise(498), 100, 3));
    EXPECT_EQ(book.depth(Side::BUY, 2).size(), 2u);
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(500), 100, 1));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.hasBids());                   // empty level is gone
}

TEST(OrderBook, CancelUnknownOrderReturnsFalse) {
    OrderBook book(0);
    EXPECT_FALSE(book.cancel(999));
}

TEST(OrderBook, CancelBestBidPromotesNextLevel) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(500), 100, 1));
    book.add(lim(2, Side::BUY, toPaise(499), 100, 2));
    book.cancel(1);
    EXPECT_EQ(book.bestBidPrice(), toPaise(499));
}

TEST(OrderBook, CancelOneOfManyKeepsLevel) {
    OrderBook book(0);
    book.add(lim(1, Side::BUY, toPaise(500), 100, 1));
    book.add(lim(2, Side::BUY, toPaise(500), 200, 2));
    book.cancel(1);
    Depth expected{{toPaise(500), 200}};
    EXPECT_EQ(book.depth(Side::BUY, 10), expected);
    EXPECT_EQ(book.front(Side::BUY).id, 2u);
}

TEST(OrderBook, PartialFillKeepsOrderInBook) {
    OrderBook book(0);
    book.add(lim(5, Side::SELL, toPaise(501), 100, 1));
    book.fillFront(Side::SELL, 40);
    EXPECT_TRUE(book.contains(5));
    EXPECT_EQ(book.front(Side::SELL).remaining, 60);
    EXPECT_EQ(book.orderCount(), 1u);
}

TEST(OrderBook, FullFillRemovesOrderAndLevel) {
    OrderBook book(0);
    book.add(lim(5, Side::SELL, toPaise(501), 100, 1));
    book.fillFront(Side::SELL, 100);
    EXPECT_FALSE(book.contains(5));                 // gone from the index too
    EXPECT_FALSE(book.hasAsks());
}