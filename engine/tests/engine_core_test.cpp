#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>
#include "EngineCore.hpp"

using namespace exch;

static Command newLimit(UserId u, Side s, Price px, Qty q, std::uint64_t ref = 0) {
    Command c{};
    c.type = CommandType::NEW_ORDER;
    c.side = s;
    c.orderType = OrderType::LIMIT;
    c.symbol = 0;
    c.user = u;
    c.price = px;
    c.qty = q;
    c.clientRef = ref;
    return c;
}
static Command newMarket(UserId u, Side s, Qty q, std::uint64_t ref = 0) {
    Command c = newLimit(u, s, 0, q, ref);
    c.orderType = OrderType::MARKET;
    return c;
}
static Command cancelCmd(UserId u, OrderId id, std::uint64_t ref = 0) {
    Command c{};
    c.type = CommandType::CANCEL_ORDER;
    c.symbol = 0;
    c.user = u;
    c.orderId = id;
    c.clientRef = ref;
    return c;
}

// alice: Rs 1,00,000 cash. bob: Rs 1,00,000 cash plus 1000 shares.
class EngineCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        alice = core.exchange().accounts().createAccount(toPaise(100000));
        bob   = core.exchange().accounts().createAccount(toPaise(100000));
        core.exchange().accounts().grantShares(bob, 0, 1000, toPaise(400));
    }
    std::vector<Event> run(const Command& c) {
        std::vector<Event> out;
        core.process(c, [&](const Event& e) { out.push_back(e); });
        return out;
    }
    EngineCore core{1};
    UserId alice = 0;
    UserId bob   = 0;
};

TEST_F(EngineCoreTest, AcceptedOrderProducesOneAck) {
    auto ev = run(newLimit(alice, Side::BUY, toPaise(500), 100, 42));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);
    EXPECT_EQ(ev[0].status, OrderStatus::NEW);
    EXPECT_EQ(ev[0].remaining, 100);
    EXPECT_EQ(ev[0].user, alice);
    EXPECT_EQ(ev[0].clientRef, 42u);
    EXPECT_NE(ev[0].orderId, 0u);
}

TEST_F(EngineCoreTest, RejectedOrderProducesOnlyRejectEvent) {
    auto ev = run(newLimit(99, Side::BUY, toPaise(500), 100, 5));   // user 99 does not exist
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::ORDER_REJECTED);
    EXPECT_EQ(ev[0].reason, RejectReason::UNKNOWN_USER);
    EXPECT_EQ(ev[0].status, OrderStatus::REJECTED);
    EXPECT_EQ(ev[0].clientRef, 5u);
}

TEST_F(EngineCoreTest, MatchProducesAckThenTrade) {
    run(newLimit(bob, Side::SELL, toPaise(500), 100));
    auto ev = run(newLimit(alice, Side::BUY, toPaise(500), 100, 7));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);
    EXPECT_EQ(ev[0].status, OrderStatus::FILLED);
    EXPECT_EQ(ev[0].remaining, 0);
    EXPECT_EQ(ev[1].type, EventType::TRADE);
    EXPECT_EQ(ev[1].trade.qty, 100);
    EXPECT_EQ(ev[1].trade.price, toPaise(500));
    EXPECT_EQ(ev[1].trade.buyerId, alice);
    EXPECT_EQ(ev[1].trade.sellerId, bob);
    EXPECT_EQ(ev[1].clientRef, 7u);
}

TEST_F(EngineCoreTest, PartialFillAckCarriesRemainder) {
    run(newLimit(bob, Side::SELL, toPaise(500), 40));
    auto ev = run(newLimit(alice, Side::BUY, toPaise(500), 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].status, OrderStatus::PARTIAL);
    EXPECT_EQ(ev[0].remaining, 60);
    EXPECT_EQ(ev[1].trade.qty, 40);
}

TEST_F(EngineCoreTest, EveryTradeGetsItsOwnEvent) {
    run(newLimit(bob, Side::SELL, toPaise(501), 100));
    run(newLimit(bob, Side::SELL, toPaise(502), 100));
    auto ev = run(newMarket(alice, Side::BUY, 150));
    ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);
    EXPECT_EQ(ev[0].status, OrderStatus::FILLED);
    EXPECT_EQ(ev[1].trade.price, toPaise(501));
    EXPECT_EQ(ev[1].trade.qty, 100);
    EXPECT_EQ(ev[2].trade.price, toPaise(502));
    EXPECT_EQ(ev[2].trade.qty, 50);
}

TEST_F(EngineCoreTest, CancelProducesCancelledEvent) {
    auto placed = run(newLimit(alice, Side::BUY, toPaise(500), 100));
    const OrderId id = placed[0].orderId;
    auto ev = run(cancelCmd(alice, id, 9));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::ORDER_CANCELLED);
    EXPECT_EQ(ev[0].orderId, id);
    EXPECT_EQ(ev[0].clientRef, 9u);
    EXPECT_EQ(core.exchange().engine().book(0).orderCount(), 0u);
}

TEST_F(EngineCoreTest, CancelOfUnknownOrderIsRejected) {
    auto ev = run(cancelCmd(alice, makeOrderId(0, 999)));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::CANCEL_REJECTED);
}

TEST_F(EngineCoreTest, CancelByWrongUserIsRejected) {
    auto placed = run(newLimit(alice, Side::BUY, toPaise(500), 100));
    const OrderId id = placed[0].orderId;
    auto ev = run(cancelCmd(bob, id));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::CANCEL_REJECTED);
    EXPECT_TRUE(core.exchange().engine().book(0).contains(id));   // still resting
}

// ---- determinism: the journal replay in a later step depends on this ----

static bool same(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.status == b.status &&
           a.side == b.side && a.symbol == b.symbol && a.user == b.user &&
           a.orderId == b.orderId && a.price == b.price && a.qty == b.qty &&
           a.remaining == b.remaining && a.clientRef == b.clientRef &&
           a.trade.tradeId == b.trade.tradeId && a.trade.symbolId == b.trade.symbolId &&
           a.trade.price == b.trade.price && a.trade.qty == b.trade.qty &&
           a.trade.buyOrderId == b.trade.buyOrderId &&
           a.trade.sellOrderId == b.trade.sellOrderId &&
           a.trade.buyerId == b.trade.buyerId && a.trade.sellerId == b.trade.sellerId &&
           a.trade.seq == b.trade.seq;
}

static std::unique_ptr<EngineCore> makeSeededCore() {
    auto core = std::make_unique<EngineCore>(1);
    for (int i = 0; i < 3; ++i) {
        const UserId u = core->exchange().accounts().createAccount(toPaise(1'000'000));
        core->exchange().accounts().grantShares(u, 0, 5000, toPaise(500));
    }
    return core;
}

TEST(EngineCoreProperty, SameCommandsGiveIdenticalEvents) {
    std::mt19937 rng(7);
    std::vector<Command> cmds;
    for (int i = 0; i < 5000; ++i) {
        Command c{};
        c.user = rng() % 3;
        c.symbol = 0;
        c.clientRef = static_cast<std::uint64_t>(i);
        const unsigned roll = rng() % 100;
        if (roll < 15) {                                   // cancels; many target ids that don't exist
            c.type = CommandType::CANCEL_ORDER;
            c.orderId = makeOrderId(0, 1 + rng() % 400);
        } else {
            c.type = CommandType::NEW_ORDER;
            c.side = (rng() & 1) ? Side::BUY : Side::SELL;
            c.qty = 1 + rng() % 100;
            if (roll < 80) {
                c.orderType = OrderType::LIMIT;
                c.price = toPaise(490 + rng() % 21);
            } else {
                c.orderType = OrderType::MARKET;
            }
        }
        cmds.push_back(c);
    }

    auto run = [&](EngineCore& core) {
        std::vector<Event> out;
        for (const Command& c : cmds)
            core.process(c, [&](const Event& e) { out.push_back(e); });
        return out;
    };

    auto a = makeSeededCore();
    auto b = makeSeededCore();
    const std::vector<Event> ea = run(*a);
    const std::vector<Event> eb = run(*b);

    ASSERT_EQ(ea.size(), eb.size());
    std::size_t trades = 0;
    for (std::size_t i = 0; i < ea.size(); ++i) {
        ASSERT_TRUE(same(ea[i], eb[i])) << "streams diverge at event " << i;
        if (ea[i].type == EventType::TRADE) ++trades;
    }
    EXPECT_GT(trades, 0u);                                 // the test isn't vacuous
}
// ---------- queries ----------

static Command queryAccountCmd(UserId u, std::uint64_t ref = 0) {
    Command c{};
    c.type      = CommandType::QUERY_ACCOUNT;
    c.user      = u;
    c.clientRef = ref;
    return c;
}
static Command queryBookCmd(SymbolId s, Qty depth, std::uint64_t ref = 0) {
    Command c{};
    c.type      = CommandType::QUERY_BOOK;
    c.symbol    = s;
    c.qty       = depth;
    c.clientRef = ref;
    return c;
}

TEST_F(EngineCoreTest, AccountQueryReportsCashAndEveryPosition) {
    run(newLimit(alice, Side::BUY, toPaise(500), 100));        // reserves Rs 50,000
    auto ev = run(queryAccountCmd(alice, 7));
    ASSERT_EQ(ev.size(), 3u);                                  // balance, the one position, done
    EXPECT_EQ(ev[0].type, EventType::ACCOUNT_BALANCE);
    EXPECT_EQ(ev[0].user, alice);
    EXPECT_EQ(ev[0].price, toPaise(100000));                   // cash
    EXPECT_EQ(ev[0].qty, toPaise(500) * 100);                  // reserved cash
    EXPECT_EQ(ev[1].type, EventType::ACCOUNT_POSITION);
    EXPECT_EQ(ev[1].symbol, 0);
    EXPECT_EQ(ev[1].qty, 0);
    EXPECT_EQ(ev[2].type, EventType::QUERY_DONE);
    for (const Event& e : ev) EXPECT_EQ(e.clientRef, 7u);
}

TEST_F(EngineCoreTest, AccountQueryShowsHoldingsReservedSharesAndCostBasis) {
    run(newLimit(bob, Side::SELL, toPaise(500), 100));         // reserves 100 of bob's 1000 shares
    auto ev = run(queryAccountCmd(bob));
    ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[1].qty, 1000);
    EXPECT_EQ(ev[1].remaining, 100);                           // reserved by the open sell
    EXPECT_EQ(ev[1].price, toPaise(400) * 1000);               // total cost basis
}

TEST_F(EngineCoreTest, AccountQueryForAnUnknownUserIsRejected) {
    auto ev = run(queryAccountCmd(99, 5));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::QUERY_REJECTED);
    EXPECT_EQ(ev[0].reason, RejectReason::UNKNOWN_USER);
    EXPECT_EQ(ev[0].clientRef, 5u);
}

TEST_F(EngineCoreTest, BookQueryListsBidsThenAsksBestFirst) {
    run(newLimit(alice, Side::BUY, toPaise(498), 10));
    run(newLimit(alice, Side::BUY, toPaise(499), 20));
    run(newLimit(alice, Side::BUY, toPaise(499), 5));          // same level as the previous order
    run(newLimit(bob, Side::SELL, toPaise(502), 7));
    run(newLimit(bob, Side::SELL, toPaise(501), 3));
    auto ev = run(queryBookCmd(0, 10, 9));

    ASSERT_EQ(ev.size(), 5u);                                  // 2 bid levels, 2 ask levels, done
    EXPECT_EQ(ev[0].type, EventType::BOOK_LEVEL);
    EXPECT_EQ(ev[0].side, Side::BUY);
    EXPECT_EQ(ev[0].price, toPaise(499));
    EXPECT_EQ(ev[0].qty, 25);                                  // the two orders at that price, added up
    EXPECT_EQ(ev[1].side, Side::BUY);
    EXPECT_EQ(ev[1].price, toPaise(498));
    EXPECT_EQ(ev[1].qty, 10);
    EXPECT_EQ(ev[2].side, Side::SELL);
    EXPECT_EQ(ev[2].price, toPaise(501));
    EXPECT_EQ(ev[2].qty, 3);
    EXPECT_EQ(ev[3].side, Side::SELL);
    EXPECT_EQ(ev[3].price, toPaise(502));
    EXPECT_EQ(ev[3].qty, 7);
    EXPECT_EQ(ev[4].type, EventType::QUERY_DONE);
    for (const Event& e : ev) {
        EXPECT_EQ(e.clientRef, 9u);
        EXPECT_EQ(e.symbol, 0);
    }
}

TEST_F(EngineCoreTest, BookQueryStopsAtTheRequestedDepthAndClampsOddValues) {
    for (int i = 0; i < 5; ++i) run(newLimit(alice, Side::BUY, toPaise(490 + i), 1));   // 5 bid levels

    auto two = run(queryBookCmd(0, 2));                        // 2 bid levels + done
    ASSERT_EQ(two.size(), 3u);
    EXPECT_EQ(two[0].price, toPaise(494));
    EXPECT_EQ(two[1].price, toPaise(493));

    EXPECT_EQ(run(queryBookCmd(0, 0)).size(), 2u);             // a depth below 1 means 1
    EXPECT_EQ(run(queryBookCmd(0, -5)).size(), 2u);
    EXPECT_EQ(run(queryBookCmd(0, 100000)).size(), 6u);        // capped at 200, and the book has only 5
}

TEST_F(EngineCoreTest, BookQueryForAnUnknownSymbolIsRejected) {
    auto ev = run(queryBookCmd(5, 10, 3));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::QUERY_REJECTED);
    EXPECT_EQ(ev[0].reason, RejectReason::UNKNOWN_SYMBOL);
    EXPECT_EQ(ev[0].clientRef, 3u);
}

TEST_F(EngineCoreTest, QueriesChangeNothing) {
    run(newLimit(alice, Side::BUY, toPaise(500), 10));
    const Account before = core.exchange().accounts().account(alice);
    const std::size_t restingBefore = core.exchange().engine().book(0).orderCount();

    for (int i = 0; i < 3; ++i) {
        run(queryAccountCmd(alice));
        run(queryBookCmd(0, 10));
        run(queryAccountCmd(99));
        run(queryBookCmd(5, 10));
    }

    const Account& after = core.exchange().accounts().account(alice);
    EXPECT_EQ(after.cash, before.cash);
    EXPECT_EQ(after.reservedCash, before.reservedCash);
    EXPECT_EQ(after.positions[0].qty, before.positions[0].qty);
    EXPECT_EQ(core.exchange().engine().book(0).orderCount(), restingBefore);
    auto ev = run(newLimit(alice, Side::BUY, toPaise(499), 1));
    EXPECT_EQ(ev[0].orderId, makeOrderId(0, 2));               // the queries did not use up an order id
}

// ---------- engine identity ----------

static Command queryInfoCmd(std::uint64_t ref = 0) {
    Command c{};
    c.type      = CommandType::QUERY_INFO;
    c.clientRef = ref;
    return c;
}

TEST_F(EngineCoreTest, QueryInfoReportsTheEngineIdAndSymbolCount) {
    auto ev = run(queryInfoCmd(4));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].type, EventType::ENGINE_INFO);
    EXPECT_EQ(ev[0].qty, 0);                                   // no journal: no identity
    EXPECT_EQ(ev[0].remaining, 1);                             // this core has one symbol
    EXPECT_EQ(ev[0].clientRef, 4u);
    EXPECT_EQ(ev[1].type, EventType::QUERY_DONE);

    core.setEngineId(123456789);
    ev = run(queryInfoCmd(5));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].qty, 123456789);
}
