#include <gtest/gtest.h>
#include <random>
#include <utility>
#include <vector>
#include "Exchange.hpp"

using namespace exch;

// Two users: alice (Rs 1,00,000 cash) and bob (Rs 1,00,000 + 1000 shares). One stock.
class ExchangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        alice = ex.accounts().createAccount(toPaise(100000));
        bob   = ex.accounts().createAccount(toPaise(100000));
        ex.accounts().grantShares(bob, 0, 1000, toPaise(400));
    }
    ExchangeResult limit(UserId u, Side s, Price px, Qty q) {
        return ex.submit(u, 0, s, OrderType::LIMIT, px, q);
    }
    ExchangeResult market(UserId u, Side s, Qty q) {
        return ex.submit(u, 0, s, OrderType::MARKET, 0, q);
    }
    const Account& acct(UserId u) const { return ex.accounts().account(u); }

    Exchange ex{1};
    UserId alice = 0;
    UserId bob   = 0;
};

// ---------- validation ----------
TEST_F(ExchangeTest, UnknownUserRejected) {
    auto r = ex.submit(99, 0, Side::BUY, OrderType::LIMIT, toPaise(500), 10);
    EXPECT_EQ(r.order.status, OrderStatus::REJECTED);
    EXPECT_EQ(r.reason, RejectReason::UNKNOWN_USER);
    EXPECT_TRUE(r.trades.empty());
}

TEST_F(ExchangeTest, UnknownSymbolRejected) {
    auto r = ex.submit(alice, 5, Side::BUY, OrderType::LIMIT, toPaise(500), 10);
    EXPECT_EQ(r.reason, RejectReason::UNKNOWN_SYMBOL);
}

TEST_F(ExchangeTest, BadQuantityRejected) {
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(500), 0).reason, RejectReason::BAD_QUANTITY);
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(500), -5).reason, RejectReason::BAD_QUANTITY);
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(500), kMaxQty + 1).reason,
              RejectReason::BAD_QUANTITY);
}

TEST_F(ExchangeTest, BadLimitPriceRejected) {
    EXPECT_EQ(limit(alice, Side::BUY, 0, 10).reason, RejectReason::BAD_PRICE);
    EXPECT_EQ(limit(alice, Side::BUY, kMaxPrice + 1, 10).reason, RejectReason::BAD_PRICE);
}

// ---------- reservations ----------
TEST_F(ExchangeTest, BuyOrderReservesCash) {
    auto r = limit(alice, Side::BUY, toPaise(500), 100);
    EXPECT_EQ(r.reason, RejectReason::NONE);
    EXPECT_EQ(acct(alice).reservedCash, toPaise(500) * 100);
    EXPECT_EQ(acct(alice).cash, toPaise(100000));      // nothing spent yet
    EXPECT_EQ(acct(alice).availableCash(), toPaise(100000) - toPaise(500) * 100);
}

TEST_F(ExchangeTest, BuyRejectedWhenFundsInsufficient) {
    auto r = limit(alice, Side::BUY, toPaise(500), 300);   // needs Rs 1,50,000
    EXPECT_EQ(r.order.status, OrderStatus::REJECTED);
    EXPECT_EQ(r.reason, RejectReason::INSUFFICIENT_FUNDS);
    EXPECT_EQ(acct(alice).reservedCash, 0);
    EXPECT_EQ(ex.engine().book(0).orderCount(), 0u);
}

// The double-spend scenario: two orders, money for only one.
TEST_F(ExchangeTest, DoubleSpendIsPrevented) {
    auto first  = limit(alice, Side::BUY, toPaise(500), 150);   // Rs 75,000
    auto second = limit(alice, Side::BUY, toPaise(500), 150);   // another Rs 75,000
    EXPECT_EQ(first.reason, RejectReason::NONE);
    EXPECT_EQ(second.reason, RejectReason::INSUFFICIENT_FUNDS);
    EXPECT_EQ(second.order.status, OrderStatus::REJECTED);
    EXPECT_EQ(ex.engine().book(0).orderCount(), 1u);
}

TEST_F(ExchangeTest, SellRejectedWithoutShares) {
    auto r = limit(alice, Side::SELL, toPaise(500), 10);
    EXPECT_EQ(r.reason, RejectReason::INSUFFICIENT_SHARES);
}

TEST_F(ExchangeTest, SellOrderReservesShares) {
    limit(bob, Side::SELL, toPaise(500), 100);
    const Position& p = acct(bob).positions[0];
    EXPECT_EQ(p.qty, 1000);
    EXPECT_EQ(p.reservedQty, 100);
    EXPECT_EQ(p.available(), 900);
}

TEST_F(ExchangeTest, SameSharesCannotBeSoldTwice) {
    EXPECT_EQ(limit(bob, Side::SELL, toPaise(500), 600).reason, RejectReason::NONE);
    EXPECT_EQ(limit(bob, Side::SELL, toPaise(500), 600).reason,
              RejectReason::INSUFFICIENT_SHARES);               // only 400 left
}

// ---------- settlement ----------
TEST_F(ExchangeTest, TradeSettlesCashAndShares) {
    limit(bob, Side::SELL, toPaise(500), 100);
    auto r = limit(alice, Side::BUY, toPaise(500), 100);
    ASSERT_EQ(r.trades.size(), 1u);

    EXPECT_EQ(acct(alice).cash, toPaise(100000) - toPaise(500) * 100);
    EXPECT_EQ(acct(alice).reservedCash, 0);
    EXPECT_EQ(acct(alice).positions[0].qty, 100);
    EXPECT_EQ(acct(alice).positions[0].avgCost(), toPaise(500));

    EXPECT_EQ(acct(bob).cash, toPaise(100000) + toPaise(500) * 100);
    EXPECT_EQ(acct(bob).positions[0].qty, 900);
    EXPECT_EQ(acct(bob).positions[0].reservedQty, 0);
    EXPECT_EQ(acct(bob).positions[0].avgCost(), toPaise(400));  // unchanged by selling
}

TEST_F(ExchangeTest, PriceImprovementReleasesExcessReserve) {
    limit(bob, Side::SELL, toPaise(501), 100);
    auto r = limit(alice, Side::BUY, toPaise(505), 100);        // willing to pay 505
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, toPaise(501));
    EXPECT_EQ(acct(alice).reservedCash, 0);                     // excess released
    EXPECT_EQ(acct(alice).cash, toPaise(100000) - toPaise(501) * 100);
}

TEST_F(ExchangeTest, PartialFillKeepsRemainderReserved) {
    limit(bob, Side::SELL, toPaise(500), 40);
    auto r = limit(alice, Side::BUY, toPaise(500), 100);
    EXPECT_EQ(r.order.status, OrderStatus::PARTIAL);
    EXPECT_EQ(acct(alice).cash, toPaise(100000) - toPaise(500) * 40);
    EXPECT_EQ(acct(alice).reservedCash, toPaise(500) * 60);     // the 60 still resting
}

// ---------- cancel ----------
TEST_F(ExchangeTest, CancelReleasesReservedCash) {
    auto r = limit(alice, Side::BUY, toPaise(500), 100);
    EXPECT_TRUE(ex.cancel(alice, 0, r.order.id));
    EXPECT_EQ(acct(alice).reservedCash, 0);
    EXPECT_EQ(ex.engine().book(0).orderCount(), 0u);
}

TEST_F(ExchangeTest, CancelReleasesReservedShares) {
    auto r = limit(bob, Side::SELL, toPaise(500), 100);
    EXPECT_TRUE(ex.cancel(bob, 0, r.order.id));
    EXPECT_EQ(acct(bob).positions[0].reservedQty, 0);
}

TEST_F(ExchangeTest, CannotCancelAnotherUsersOrder) {
    auto r = limit(alice, Side::BUY, toPaise(500), 100);
    EXPECT_FALSE(ex.cancel(bob, 0, r.order.id));
    EXPECT_TRUE(ex.engine().book(0).contains(r.order.id));
}

// ---------- market orders ----------
TEST_F(ExchangeTest, MarketBuyPaysActualPrices) {
    limit(bob, Side::SELL, toPaise(501), 100);
    limit(bob, Side::SELL, toPaise(502), 100);
    auto r = market(alice, Side::BUY, 150);

    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.order.status, OrderStatus::FILLED);
    EXPECT_EQ(acct(alice).cash,
              toPaise(100000) - (toPaise(501) * 100 + toPaise(502) * 50));
    EXPECT_EQ(acct(alice).reservedCash, 0);
    EXPECT_EQ(acct(alice).positions[0].qty, 150);
}

TEST_F(ExchangeTest, MarketBuyRejectedIfBookCostExceedsFunds) {
    limit(bob, Side::SELL, toPaise(500), 500);                  // Rs 2,50,000 worth
    auto r = market(alice, Side::BUY, 500);                     // alice has Rs 1,00,000
    EXPECT_EQ(r.reason, RejectReason::INSUFFICIENT_FUNDS);
    EXPECT_TRUE(ex.engine().book(0).contains(1));               // bob's order untouched
}

TEST_F(ExchangeTest, MarketSellReleasesUnfilledShares) {
    limit(alice, Side::BUY, toPaise(500), 30);
    auto r = market(bob, Side::SELL, 100);                      // only 30 can fill

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.order.status, OrderStatus::CANCELLED);
    EXPECT_EQ(r.order.remaining, 70);
    EXPECT_EQ(acct(bob).positions[0].qty, 970);
    EXPECT_EQ(acct(bob).positions[0].reservedQty, 0);           // 70 released
}

TEST_F(ExchangeTest, MarketBuyOnEmptyBookCostsNothing) {
    auto r = market(alice, Side::BUY, 10);
    EXPECT_EQ(r.reason, RejectReason::NONE);
    EXPECT_EQ(r.order.status, OrderStatus::CANCELLED);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(acct(alice).reservedCash, 0);
}

// ---------- the big one: random orders, nothing may leak ----------
TEST(ExchangeProperty, CashAndSharesAreConserved) {
    constexpr unsigned kUsers = 4;
    const Price startCash   = toPaise(1'000'000);
    const Qty   startShares = 10'000;

    Exchange ex(1);
    for (unsigned i = 0; i < kUsers; ++i) {
        UserId u = ex.accounts().createAccount(startCash);
        ex.accounts().grantShares(u, 0, startShares, toPaise(500));
    }

    std::mt19937 rng(12345);                       // fixed seed: reproducible
    std::vector<std::pair<UserId, OrderId>> placed;

    for (int i = 0; i < 20000; ++i) {
        const UserId   user   = rng() % kUsers;
        const unsigned action = rng() % 100;

        if (action < 15 && !placed.empty()) {      // 15%: cancel a random old order
            auto [owner, id] = placed[rng() % placed.size()];
            ex.cancel(owner, 0, id);
            continue;
        }
        const Side side = (rng() % 2) ? Side::BUY : Side::SELL;
        const Qty  qty  = 1 + rng() % 200;
        auto r = (action < 80)                     // 65% limit, 20% market
            ? ex.submit(user, 0, side, OrderType::LIMIT, toPaise(490 + rng() % 21), qty)
            : ex.submit(user, 0, side, OrderType::MARKET, 0, qty);
        if (r.reason == RejectReason::NONE) placed.emplace_back(user, r.order.id);
    }

    // Cancel everything still resting, so every reservation must come back.
    for (const auto& [owner, id] : placed) ex.cancel(owner, 0, id);

    EXPECT_EQ(ex.engine().book(0).orderCount(), 0u);
    EXPECT_EQ(ex.accounts().totalCash(),     Price(kUsers) * startCash);     // no money created or lost
    EXPECT_EQ(ex.accounts().totalShares(0),  Qty(kUsers) * startShares);     // no shares created or lost
    for (UserId u = 0; u < kUsers; ++u) {
        const Account& a = ex.accounts().account(u);
        EXPECT_EQ(a.reservedCash, 0);
        EXPECT_EQ(a.positions[0].reservedQty, 0);
        EXPECT_GE(a.cash, 0);
        EXPECT_GE(a.positions[0].qty, 0);
    }
}
TEST(ExchangeIds, EachSymbolNumbersItsOwnOrdersButIdsStayUnique) {
    Exchange ex(2);
    const UserId u = ex.accounts().createAccount(toPaise(100000));
    auto a = ex.submit(u, 0, Side::BUY, OrderType::LIMIT, toPaise(500), 1);
    auto b = ex.submit(u, 1, Side::BUY, OrderType::LIMIT, toPaise(500), 1);

    EXPECT_NE(a.order.id, b.order.id);              // unique across symbols
    EXPECT_EQ(localOrderId(a.order.id), 1u);
    EXPECT_EQ(localOrderId(b.order.id), 1u);        // but each symbol starts at 1
    EXPECT_FALSE(ex.cancel(u, 0, b.order.id));      // symbol 1's id must not hit symbol 0's book
    EXPECT_TRUE(ex.cancel(u, 1, b.order.id));
    EXPECT_TRUE(ex.cancel(u, 0, a.order.id));
}
TEST_F(ExchangeTest, PriceMustBeOnTick) {
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(500) + 3, 10).reason, RejectReason::PRICE_OFF_TICK);
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(500) + 5, 10).reason, RejectReason::NONE);
}

TEST_F(ExchangeTest, PriceMustBeInsideBand) {
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(399), 10).reason, RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(601), 10).reason, RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(400), 10).reason, RejectReason::NONE);   // edges are allowed
    EXPECT_EQ(limit(alice, Side::BUY, toPaise(600), 10).reason, RejectReason::NONE);
}
// ---------- a band per symbol ----------

TEST(ExchangeBands, EachSymbolHasItsOwnBand) {
    Exchange ex(2, {toPaise(2500), toPaise(800)});            // symbol 0: Rs 2,500; symbol 1: Rs 800
    const UserId u = ex.accounts().createAccount(toPaise(10'000'000));
    auto buy = [&](SymbolId s, Price px) {
        return ex.submit(u, s, Side::BUY, OrderType::LIMIT, px, 1).reason;
    };
    EXPECT_EQ(buy(0, toPaise(2000)), RejectReason::NONE);                 // the band edges are allowed
    EXPECT_EQ(buy(0, toPaise(3000)), RejectReason::NONE);
    EXPECT_EQ(buy(0, toPaise(1999)), RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(buy(0, toPaise(3001)), RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(buy(0, toPaise(800)),  RejectReason::PRICE_OUT_OF_BAND);    // fine for symbol 1, far too low here

    EXPECT_EQ(buy(1, toPaise(640)),  RejectReason::NONE);
    EXPECT_EQ(buy(1, toPaise(960)),  RejectReason::NONE);
    EXPECT_EQ(buy(1, toPaise(639)),  RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(buy(1, toPaise(2500)), RejectReason::PRICE_OUT_OF_BAND);
}

TEST(ExchangeBands, TradesHappenAtEachSymbolsOwnPrices) {
    Exchange ex(2, {toPaise(2500), toPaise(800)});
    const UserId seller = ex.accounts().createAccount(toPaise(1'000'000));
    const UserId buyer  = ex.accounts().createAccount(toPaise(1'000'000));
    ex.accounts().grantShares(seller, 0, 100, toPaise(2400));
    ex.accounts().grantShares(seller, 1, 100, toPaise(700));

    ex.submit(seller, 0, Side::SELL, OrderType::LIMIT, toPaise(2500), 10);
    ex.submit(seller, 1, Side::SELL, OrderType::LIMIT, toPaise(800), 10);
    const auto r0 = ex.submit(buyer, 0, Side::BUY, OrderType::LIMIT, toPaise(2500), 10);
    const auto r1 = ex.submit(buyer, 1, Side::BUY, OrderType::LIMIT, toPaise(800), 4);

    ASSERT_EQ(r0.trades.size(), 1u);
    EXPECT_EQ(r0.trades[0].price, toPaise(2500));
    EXPECT_EQ(r0.order.status, OrderStatus::FILLED);
    ASSERT_EQ(r1.trades.size(), 1u);
    EXPECT_EQ(r1.trades[0].price, toPaise(800));
    EXPECT_EQ(ex.engine().book(0).orderCount(), 0u);
    EXPECT_EQ(ex.engine().book(1).orderCount(), 1u);          // 6 of symbol 1's 10 shares still rest
    EXPECT_EQ(ex.accounts().account(buyer).cash,
              toPaise(1'000'000) - toPaise(2500) * 10 - toPaise(800) * 4);
}

TEST(ExchangeBands, BadReferencePricesAreRefused) {
    EXPECT_THROW(Exchange(2, {toPaise(500)}), std::invalid_argument);        // one price for two symbols
    EXPECT_THROW(Exchange(1, {0}), std::invalid_argument);                   // not positive
    EXPECT_THROW(Exchange(1, {toPaise(500) + 1}), std::invalid_argument);    // off the tick
    EXPECT_THROW(Exchange(1, {toPaise(500) + 5}), std::invalid_argument);    // band edges would be off the tick
    EXPECT_NO_THROW(Exchange(1, {toPaise(2500)}));
}
TEST(ExchangeBands, ReferencePricesCanBeReadBack) {
    Exchange defaults(3);
    EXPECT_EQ(defaults.numSymbols(), 3u);
    for (SymbolId s = 0; s < 3; ++s) EXPECT_EQ(defaults.referencePrice(s), toPaise(500));
    Exchange custom(2, {toPaise(2500), toPaise(800)});
    EXPECT_EQ(custom.referencePrice(0), toPaise(2500));
    EXPECT_EQ(custom.referencePrice(1), toPaise(800));
}